#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>

#define PCM_DEV        "hw:0,0"
#define SAMPLE_RATE    (48000)
#define CHANNELS       (2)
#define FORMAT         SND_PCM_FORMAT_S16_LE
#define FRAME_SIZE     (240U)
#define FRAME_BYTES    (960U)
#define BUF_SIZE       (960U)
#define AUDIO_BUF_LEN  (5U)
#define PCM_WAIT_MS    (5U)

/* debugfs nodes */
#define PLAYBACK_DBGFS "/sys/kernel/debug/audio_fe_g0/playback_buf"
#define CAPTURE_DBGFS  "/sys/kernel/debug/audio_fe_g0/capture_buf"

/* wav file header */
typedef struct {
	char           id[4];
	uint32_t       size;
	char           wave[4];
} riff_header_t;

/* chunk header */
typedef struct {
	char           id[4];
	uint32_t       size;
} chunk_header_t;

/* circular buffer for audio data */
typedef struct {
	uint8_t         buffer[AUDIO_BUF_LEN][FRAME_BYTES];
	uint32_t        rd_id;
	uint32_t        wr_id;
} file_rd_buf_t;

/* private struct for application */
typedef struct {
	file_rd_buf_t   buf;
	FILE            *fin;
	FILE            *fout;
	uint32_t        remaining;
	snd_pcm_t       *pcm;
	int             dbgfs_fd;
	pthread_mutex_t lock;
	bool            terminate;
	sem_t           start_pcm;
	sem_t           start_out;
} test_priv_t;

/* Global variables */
test_priv_t *g_priv;

/* signal handler for SIGINT */
void handle_sigint(int sig)
{
	printf("CTRL+C Pressed, Stopping application..\n");
	pthread_mutex_lock(&g_priv->lock);
	g_priv->terminate = true;
	pthread_mutex_unlock(&g_priv->lock);
}

/* print pcm error */
static void print_pcm_error(int err, int count)
{
	if (err == -EPIPE)
		printf("error: xrun (count: %u)\n", count);
	else if (err == -ESTRPIPE)
		printf("error: suspended (count: %u)\n", count);
	else
		printf("error: unknown(%d) (count: %u)\n", err, count);
}

/* read audio data from circular buffer */
static bool read_buf(file_rd_buf_t *buf, uint8_t *data)
{
	if (buf->rd_id == buf->wr_id) {
		/* buffer empty */
		return false;
	}

	memcpy(data, &buf->buffer[buf->rd_id][0], FRAME_BYTES);
	buf->rd_id = (buf->rd_id + 1) % AUDIO_BUF_LEN;
	return true;
}

/* write audio data into circular buffer from a file */
static bool write_buf_from_file(file_rd_buf_t *buf, FILE *fin, size_t size)
{
	if ((buf->wr_id + 1) % AUDIO_BUF_LEN == buf->rd_id) {
		/* buffer full */
		return false;
	}

	if (fread(&buf->buffer[buf->wr_id][0], 1, size, fin) != size) {
		printf("Failed to read audio data\n");
		return false;
	}
	buf->wr_id = (buf->wr_id + 1) % AUDIO_BUF_LEN;
	return true;
}

/* write audio data into circular buffer*/
static bool write_buf(file_rd_buf_t *buf, uint8_t *data)
{
	if ((buf->wr_id + 1) % AUDIO_BUF_LEN == buf->rd_id) {
		/* buffer full */
		return false;
	}

	memcpy(&buf->buffer[buf->rd_id][0], data, FRAME_BYTES);
	buf->wr_id = (buf->wr_id + 1) % AUDIO_BUF_LEN;
	return true;
}

/* read audio data from circular buffer, and write it into a file */
static bool read_buf_to_file(file_rd_buf_t *buf, FILE *fout, size_t size)
{
	if (buf->rd_id == buf->wr_id) {
		/* buffer empty */
		return false;
	}

	if (fwrite(&buf->buffer[buf->wr_id][0], 1, size, fout) != size) {
		printf("Failed to write out data\n");
		return false;
	}
	buf->rd_id = (buf->rd_id + 1) % AUDIO_BUF_LEN;
	return true;
}

/* input file read thread for playback test */
static void *playback_input_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	uint32_t remaining = priv->remaining;
	bool terminate = false;
	uint32_t count = 0;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	printf("Start %s\n", __func__);

	/* check for remaining data from input file */
	while (remaining > 0) {
		struct timespec start, end;
		long time_us;
		size_t this_block;

		clock_gettime(CLOCK_MONOTONIC, &start);

		/* check for terminate flag */
		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		this_block =
			(remaining > FRAME_BYTES) ? FRAME_BYTES : remaining;

		/* read data from file, and write it into circular buffer */
		if (!write_buf_from_file(&priv->buf, priv->fin, this_block)) {
			usleep(100);
			continue;
		}

		remaining -= (uint32_t)this_block;

		count++;

		/* time profiling */
		clock_gettime(CLOCK_MONOTONIC, &end);
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_nsec - start.tv_nsec) / 1000;

		if ((time_us < tmin_us) || (!tmin_us))
			tmin_us = time_us;

		if (time_us > tmax_us)
			tmax_us = time_us;

		tavg_us = tavg_us + ((time_us - tavg_us) / count);

		if (count == (AUDIO_BUF_LEN - 1)) {
			/* signal pcm thread */
			sem_post(&priv->start_pcm);
		}
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d\n",
			__func__, tmax_us, tmin_us, tavg_us, count);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/* PCM thread for playback test */
static void *playback_pcm_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	snd_pcm_t *pcm = priv->pcm;
	bool terminate = false;
	bool started = false;
	char *mmap_buf;
	uint32_t count = 0;
	snd_pcm_uframes_t offset, mmap_frames = FRAME_SIZE;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	sem_wait(&priv->start_pcm);
	printf("Start %s\n", __func__);

	while (true) {
		const snd_pcm_channel_area_t *areas;
		snd_pcm_sframes_t avail;
		int err;
		struct timespec start, end;
		long time_us;
		bool ret;

		clock_gettime(CLOCK_MONOTONIC, &start);

		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		/* check for available audio frames */
		avail = snd_pcm_avail_update(pcm);
		if (started && !avail) {
			/* wait for PCM_WAIT_MS to get available frames */
			err = snd_pcm_wait(pcm, PCM_WAIT_MS);
			if (err == 0){
				/* timeout, no available frames */
				continue;
			}

			if (err < 0) {
				print_pcm_error(err, count);

				/* Exit from application */
				break;
			}
		}

		if (started && (avail < 0)) {
			printf("snd_pcm_avail_update error (count: %u)\n",
					count);
			print_pcm_error(avail, count);

			/* Exit from application */
			break;
		}

		mmap_frames = FRAME_SIZE;

		/* mmap pcm buffer */
		err = snd_pcm_mmap_begin(pcm, &areas, &offset, &mmap_frames);
		if (err < 0) {
			printf("ERROR: mmap_begin failed: %s (count: %u)\n",
					snd_strerror(err), count);

			/* Exit from application */
			break;
		}

		if ((mmap_frames != FRAME_SIZE) || (offset % FRAME_SIZE)) {
			/* Wrong frame size or wrong offset */
			snd_pcm_mmap_commit(pcm, offset, 0);
			continue;
		}

		mmap_buf = (char*)areas[0].addr + offset * 4;

		/* read audio data from circular buffer into mmaped area */
		ret = read_buf(&priv->buf, mmap_buf);
		if (!ret) {
			printf("input audio data not available (count: %u)\n",
					count);
			snd_pcm_mmap_commit(pcm, offset, 0);
			continue;
		}

		err = snd_pcm_mmap_commit(pcm, offset, mmap_frames);
		if (err < 0) {
			printf("%s: snd_pcm_mmap_commit failed (count: %u)\n",
					__func__, count);

			/* Exit from application */
			break;
		}

		if (!started) {
			/* start playback if it is not done yet */
			snd_pcm_start(pcm);
			started = true;
		}

		count++;

		/* time profiling */
		clock_gettime(CLOCK_MONOTONIC, &end);
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_nsec - start.tv_nsec) / 1000;

		if ((time_us < tmin_us) || (!tmin_us))
			tmin_us = time_us;

		if (time_us > tmax_us)
			tmax_us = time_us;

		tavg_us = tavg_us + ((time_us - tavg_us) / count);

		if (count == 1) {
			/* signal out thread */
			sem_post(&priv->start_out);
		}
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d\n",
			__func__, tmax_us, tmin_us, tavg_us, count);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/* output file write thread for playback test */
static void *playback_output_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	bool terminate = false;
	uint32_t count = 0;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;
	char *audio_data = malloc(FRAME_BYTES);

	sem_wait(&priv->start_out);
	printf("Start %s\n", __func__);

	while (true) {
		struct timespec start, end;
		long time_us;
		ssize_t rc;

		clock_gettime(CLOCK_MONOTONIC, &start);

		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		/* read buffer via debugfs */
		if (read(priv->dbgfs_fd, audio_data, FRAME_BYTES) < 0) {
			usleep(100);
			continue;
		}

		if (fwrite(audio_data, 1, FRAME_BYTES, priv->fout)
				!= FRAME_BYTES) {
			printf("Failed to write data (count: %u)\n", count);

			/* Exit from application */
			break;
		}

		count++;

		/* time profiling */
		clock_gettime(CLOCK_MONOTONIC, &end);
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_nsec - start.tv_nsec) / 1000;

		if ((time_us < tmin_us) || (!tmin_us))
			tmin_us = time_us;

		if (time_us > tmax_us)
			tmax_us = time_us;

		tavg_us = tavg_us + ((time_us - tavg_us) / count);
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d\n",
			__func__, tmax_us, tmin_us, tavg_us, count);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);
	free(audio_data);

	return NULL;
}

/* Playback test */
static int test_playback(FILE *fin, FILE *fout, uint32_t remaining)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw = NULL;
	snd_pcm_uframes_t period_size, buffer_size;
	int err;
	int dbgfs_fd;
	test_priv_t *priv;
	pthread_t play_in_t, play_pcm_t, play_out_t;

	priv = malloc(sizeof(test_priv_t));
	if(!priv) {
		printf("ERROR: malloc failed for test_priv_t\n");
		return -ENOMEM;
	}
	priv->fin = fin;
	priv->fout = fout;
	priv->remaining = remaining;

	/* global variable to use in signal handler */
	g_priv = priv;

	/* open debugfs to read output buffer */
	dbgfs_fd = open(PLAYBACK_DBGFS, O_RDONLY);
	if (dbgfs_fd < 0) {
		printf("Failed to open %s (error: %s)\n",
				PLAYBACK_DBGFS, strerror(errno));
		free(priv);
		return dbgfs_fd;
	}
	priv->dbgfs_fd = dbgfs_fd;

	/* open PCM channel for playback */
	err = snd_pcm_open(&pcm, PCM_DEV, SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		printf("ERROR: cannot open PCM: %s\n", snd_strerror(err));
		close(dbgfs_fd);
		free(priv);
		return err;
	}

	/* set PCM hardware parameters */
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);

	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, FORMAT);
	snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
	snd_pcm_hw_params_set_rate(pcm, hw, SAMPLE_RATE, 0);

	period_size = FRAME_SIZE;
	buffer_size = BUF_SIZE;

	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_size, 0);
	snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_size);

	if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
		printf("ERROR: hw_params failed: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		close(dbgfs_fd);
		free(priv);
		return err;
	}
	snd_pcm_prepare(pcm);
	priv->pcm = pcm;

	priv->terminate = false;
	pthread_mutex_init(&priv->lock, NULL);
	sem_init(&priv->start_pcm, 0, 0);
	sem_init(&priv->start_out, 0, 0);

	/* Register signal handler */
	signal(SIGINT, handle_sigint);

	if (pthread_create(&play_in_t, NULL, playback_input_thread, priv)) {
		printf("Error: pthread_create for playback_input_thread failed\n");
		goto cleanup;
	}

	if (pthread_create(&play_pcm_t, NULL, playback_pcm_thread, priv)) {
		printf("Error: pthread_create for playback_pcm_thread failed\n");

		pthread_mutex_lock(&priv->lock);
		priv->terminate = true;
		pthread_mutex_unlock(&priv->lock);

		pthread_join(play_in_t, NULL);
		goto cleanup;
	}

	if (pthread_create(&play_out_t, NULL, playback_output_thread, priv)) {
		printf("Error: pthread_create for playback_output_thread failed\n");

		pthread_mutex_lock(&priv->lock);
		priv->terminate = true;
		pthread_mutex_unlock(&priv->lock);

		pthread_join(play_in_t, NULL);
		pthread_join(play_pcm_t, NULL);
		goto cleanup;
	}

	pthread_join(play_in_t, NULL);
	pthread_join(play_pcm_t, NULL);
	pthread_join(play_out_t, NULL);

cleanup:
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	close(dbgfs_fd);
	free(priv);
	sem_destroy(&priv->start_pcm);
	sem_destroy(&priv->start_out);
	pthread_mutex_destroy(&priv->lock);
	return 0;
}

/* input file read thread for capture test */
static void *capture_input_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	uint32_t remaining = priv->remaining;
	bool terminate = false;
	uint32_t count = 0;
	int dbgfs_fd = priv->dbgfs_fd;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;
	char *audio_data =  malloc(FRAME_BYTES);

	printf("Start %s\n", __func__);

	while (remaining) {
		struct timespec start, end;
		long time_us;
		size_t this_block;
		ssize_t wc;

		clock_gettime(CLOCK_MONOTONIC, &start);

		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		/* read audio data from input file */
		this_block = (remaining > FRAME_BYTES) ? FRAME_BYTES : remaining;
		if (fread(audio_data, 1, this_block, priv->fin) != this_block) {
			printf("Failed to read audio data (count: %u)\n", count);
			continue;
		}

		do {
			pthread_mutex_lock(&priv->lock);
			terminate = priv->terminate;
			pthread_mutex_unlock(&priv->lock);

			if (terminate)
				break;

			/* write audio buffer to debugfs */
			wc = write(dbgfs_fd, (const void *)audio_data, FRAME_BYTES);
			if (wc < 0)
				usleep(100);
		} while(wc < 0);

		remaining -= (uint32_t)this_block;

		count++;

		/* time profiling */
		clock_gettime(CLOCK_MONOTONIC, &end);
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_nsec - start.tv_nsec) / 1000;

		if ((time_us < tmin_us) || (!tmin_us))
			tmin_us = time_us;

		if (time_us > tmax_us)
			tmax_us = time_us;

		tavg_us = tavg_us + ((time_us - tavg_us) / count);

		if (count == (AUDIO_BUF_LEN - 1)) {
			/* signal pcm thread */
			sem_post(&priv->start_pcm);
		}
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d\n",
			__func__, tmax_us, tmin_us, tavg_us, count);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	free(audio_data);

	return NULL;
}

/* PCM thread for capture test */
static void *capture_pcm_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	snd_pcm_t *pcm = priv->pcm;
	bool terminate = false;
	bool started = false;
	char *mmap_buf;
	uint32_t count = 0;
	snd_pcm_uframes_t offset, mmap_frames = FRAME_SIZE;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	sem_wait(&priv->start_pcm);
	printf("Start %s\n", __func__);

	while (true) {
		const snd_pcm_channel_area_t *areas;
		snd_pcm_sframes_t avail;
		int err;
		struct timespec start, end;
		long time_us;
		bool ret;

		clock_gettime(CLOCK_MONOTONIC, &start);

		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		if (!started) {
			/* Start DSP capture */
			snd_pcm_start(pcm);
			started = true;
		}

		/* check for available audio frames */
		avail = snd_pcm_avail_update(pcm);
		if (!avail) {
			/* wait for PCM_WAIT_MS to get available frames */
			err = snd_pcm_wait(pcm, PCM_WAIT_MS);
			if (err == 0){
				/* timeout, no available frames */
				continue;
			}

			if (err < 0) {
				print_pcm_error(err, count);

				/* Exit from application */
				break;
			}
		}

		if (avail < 0) {
			printf("snd_pcm_avail_update error (count: %u)\n",
					count);
			print_pcm_error(avail, count);

			/* Exit from application */
			break;
		}

		mmap_frames = FRAME_SIZE;

		/* mmap pcm buffer */
		err = snd_pcm_mmap_begin(pcm, &areas, &offset, &mmap_frames);
		if (err < 0) {
			printf("ERROR: mmap_begin failed: %s (count: %u)\n",
					snd_strerror(err), count);

			/* Exit from application */
			break;
		}

		if ((mmap_frames != FRAME_SIZE) || (offset % FRAME_SIZE)) {
			/* Wrong frame size or wrong offset */
			snd_pcm_mmap_commit(pcm, offset, 0);
			continue;
		}

		mmap_buf = (char*)areas[0].addr +( offset * 4);

		ret = write_buf(&priv->buf, mmap_buf);
		if (!ret) {
			printf("Failed to write data (count: %u)\n", count);
			snd_pcm_mmap_commit(pcm, offset, 0);
			continue;
		}

		err = snd_pcm_mmap_commit(pcm, offset, mmap_frames);
		if (err < 0) {
			printf("%s: snd_pcm_mmap_commit failed (count: %u)\n",
					__func__, count);

			/* Exit from application */
			break;
		}

		count++;

		/* time profiling */
		clock_gettime(CLOCK_MONOTONIC, &end);
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_nsec - start.tv_nsec) / 1000;

		if ((time_us < tmin_us) || (!tmin_us))
			tmin_us = time_us;

		if (time_us > tmax_us)
			tmax_us = time_us;

		tavg_us = tavg_us + ((time_us - tavg_us) / count);

		if (count == 1) {
			/* signal out thread */
			sem_post(&priv->start_out);
		}
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d\n",
			__func__, tmax_us, tmin_us, tavg_us, count);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/* output file write thread for capture test */
static void *capture_output_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	uint32_t remaining = priv->remaining;
	bool terminate = false;
	uint32_t count = 0;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	sem_wait(&priv->start_out);
	printf("Start %s\n", __func__);

	while (true) {
		struct timespec start, end;
		long time_us;
		size_t this_block;

		clock_gettime(CLOCK_MONOTONIC, &start);

		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		/* read audio data from circular buffer, write it to output file */
		if (!read_buf_to_file(&priv->buf, priv->fout, FRAME_BYTES)) {
			usleep(100);
			continue;
		}

		count++;

		/* time profiling */
		clock_gettime(CLOCK_MONOTONIC, &end);
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_nsec - start.tv_nsec) / 1000;

		if ((time_us < tmin_us) || (!tmin_us))
			tmin_us = time_us;

		if (time_us > tmax_us)
			tmax_us = time_us;

		tavg_us = tavg_us + ((time_us - tavg_us) / count);
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d\n",
			__func__, tmax_us, tmin_us, tavg_us, count);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/* Capture test */
static int test_capture(FILE *fin, FILE *fout, uint32_t remaining)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw = NULL;
	snd_pcm_uframes_t period_size, buffer_size;
	int err;
	int dbgfs_fd;
	bool started = false;
	bool skip_write = false;
	char *mmap_buf;
	const snd_pcm_channel_area_t *areas;
	snd_pcm_uframes_t offset, mmap_frames = FRAME_SIZE;
	size_t this_block, rd;
	test_priv_t *priv;
	pthread_t cap_in_t, cap_pcm_t, cap_out_t;

	priv = malloc(sizeof(test_priv_t));
	if(!priv) {
		printf("ERROR: malloc failed for test_priv_t\n");
		return -ENOMEM;
	}
	priv->fin = fin;
	priv->fout = fout;
	priv->remaining = remaining;

	/* global variable to use in signal handler */
	g_priv = priv;

	/* open debugfs to read output buffer */
	dbgfs_fd = open(CAPTURE_DBGFS, O_RDWR);
	if (dbgfs_fd < 0) {
		printf("Failed to open %s (error: %s)\n", CAPTURE_DBGFS, strerror(errno));
		free(priv);
		return dbgfs_fd;
	}
	priv->dbgfs_fd = dbgfs_fd;

	/* open PCM channel for capture */
	err = snd_pcm_open(&pcm, PCM_DEV, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		printf("ERROR: cannot open PCM: %s\n", snd_strerror(err));
		close(dbgfs_fd);
		free(priv);
		return err;
	}

	/* set PCM hardware parameters */
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(pcm, hw);

	snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_MMAP_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm, hw, FORMAT);
	snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS);
	snd_pcm_hw_params_set_rate(pcm, hw, SAMPLE_RATE, 0);

	period_size = FRAME_SIZE;
	buffer_size = BUF_SIZE;

	snd_pcm_hw_params_set_period_size_near(pcm, hw, &period_size, 0);
	snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer_size);

	if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
		printf("ERROR: hw_params failed: %s\n", snd_strerror(err));
		snd_pcm_close(pcm);
		close(dbgfs_fd);
		return err;
	}

	snd_pcm_prepare(pcm);
	priv->pcm = pcm;

	priv->terminate = false;
	pthread_mutex_init(&priv->lock, NULL);
	sem_init(&priv->start_pcm, 0, 0);
	sem_init(&priv->start_out, 0, 0);

	/* Register signal handler */
	signal(SIGINT, handle_sigint);

	if (pthread_create(&cap_in_t, NULL, capture_input_thread, priv)) {
		printf("pthread_create for capture_input_thread failed\n");
		goto cleanup;
	}

	if (pthread_create(&cap_pcm_t, NULL, capture_pcm_thread, priv)) {
		printf("pthread_create for capture_pcm_thread failed\n");

		pthread_mutex_lock(&priv->lock);
		priv->terminate = true;
		pthread_mutex_unlock(&priv->lock);

		pthread_join(cap_in_t, NULL);
		goto cleanup;
	}

	if (pthread_create(&cap_out_t, NULL, capture_output_thread, priv)) {
		printf("pthread_create for capture_output_thread failed\n");

		pthread_mutex_lock(&priv->lock);
		priv->terminate = true;
		pthread_mutex_unlock(&priv->lock);

		pthread_join(cap_in_t, NULL);
		pthread_join(cap_pcm_t, NULL);
		goto cleanup;
	}

	printf("%s: App started. Press CTRL+C to Stop Application.\n", __func__);

	pthread_join(cap_in_t, NULL);
	pthread_join(cap_pcm_t, NULL);
	pthread_join(cap_out_t, NULL);

cleanup:
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	close(dbgfs_fd);
	free(priv);
	sem_destroy(&priv->start_pcm);
	sem_destroy(&priv->start_out);
	pthread_mutex_destroy(&priv->lock);
	return 0;
}

/* helper function to copy non-data chunk from input file to output file */
static int copy_n_bytes(FILE *src, FILE *dst, uint32_t n)
{
	uint8_t temp[4096];
	uint32_t remaining = n;

	while (remaining > 0) {
		size_t chunk =
			(remaining > sizeof(temp)) ? sizeof(temp) : remaining;
		size_t rd = fread(temp, 1, chunk, src);
		if (rd != chunk) {
			return -1;
		}
		if (fwrite(temp, 1, chunk, dst) != chunk) {
			return -1;
		}
		remaining -= (uint32_t)chunk;
	}
	return 0;
}

/* parse header of input wav file */
int parse_header(FILE *fin, FILE *fout, uint32_t *data_size)
{
	riff_header_t riff;
	chunk_header_t ch;
	int found_data = 0;

	if (fread(&riff, 1, sizeof(riff), fin) != sizeof(riff)) {
		printf("Error: Failed to read RIFF header\n");
		return 1;
	}

	if (memcmp(riff.id, "RIFF", 4) != 0 || memcmp(riff.wave, "WAVE", 4)
			!= 0) {
		printf("Error: Not a valid WAV file\n");
		return 1;
	}

	/* Write RIFF header unchanged */
	if (fwrite(&riff, 1, sizeof(riff), fout) != sizeof(riff)) {
		printf("Error: Failed to write RIFF header\n");
		return 1;
	}

	while (fread(&ch, 1, sizeof(ch), fin) == sizeof(ch)) {
		/* Write chunk header unchanged */
		if (fwrite(&ch, 1, sizeof(ch), fout) != sizeof(ch)) {
			printf("Error: Failed to write chunk header\n");
			return 1;
		}

		if (memcmp(ch.id, "data", 4) == 0) {
			/* Found audio payload */
			*data_size = ch.size;
			found_data = 1;
			break;
		} else {
			/* Copy this entire non-data chunk unchanged */
			if (copy_n_bytes(fin, fout, ch.size) != 0) {
				printf("Error: Failed to copy chunk %.4s\n",
						ch.id);
				return 1;
			}

			/*
			 * WAV chunks are word-aligned:
			 * odd-sized chunks have 1 pad byte
			 */
			if (ch.size & 1) {
				int pad = fgetc(fin);
				if (pad == EOF) {
					printf("Failed to read padding\n");
					return 1;
				}
				if (fputc(pad, fout) == EOF) {
					printf("Failed to write padding\n");
					return 1;
				}
			}
		}
	}

	if (!found_data) {
		printf("Error: 'data' chunk not found\n");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int ret = 1;
	FILE *fin, *fout;
	uint32_t data_size;

	if (argc != 4) {
		printf("Usage: %s playback|capture <input.wav> <output.wav>\n",
				argv[0]);
		return ret;
	}

	const char *input = argv[2];
	const char *output = argv[3];

	/* open input file for read */
	fin = fopen(input, "rb");
	if (!fin) {
		printf("Failed to open %s\n", input);
		goto out;
	}

	/* open output file for write */
	fout = fopen(output, "wb");
	if (!fin) {
		printf("Failed to open %s\n", output);
		goto out;
	}

	/* parse header of input file and copy this to output file */
	if (parse_header(fin, fout, &data_size)) {
		printf("Failed to parse header\n");
		goto out;
	}

	if (!strcmp(argv[1], "playback"))
		ret = test_playback(fin, fout, data_size);
	else if (!strcmp(argv[1], "capture"))
		ret = test_capture(fin, fout, data_size);
	else
		printf("Invalid option\n");

out:
	fclose(fin);
	fclose(fout);
	return ret;
}
