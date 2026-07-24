#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
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
#define PCM_WAIT_MS    (5U)

#define CHUNK_SLOTS    (960U)//multiple of 960
#define AUDIO_BUF_LEN  (CHUNK_SLOTS * 4U)   /* 3840 slots (~3.6MB) */

/* debugfs nodes */
#define PLAYBACK_DBGFS "/sys/kernel/debug/audio_fe_g0/playback_buf"
#define CAPTURE_DBGFS  "/sys/kernel/debug/audio_fe_g0/capture_buf"

/*
 * Lightweight per-loop-iteration time profiling.
 *
 * PROFILE_START() takes the "before" timestamp, PROFILE_STOP() takes
 * the "after" timestamp and folds the elapsed time into running
 * min/max/avg stats. Kept as macros (rather than a helper function)
 * so they can be dropped inline in each thread's loop without the
 * overhead/awkwardness of passing every stat variable by pointer.
 *
 * The calling function must have these locals in scope:
 *   struct timespec start, end;
 *   long            time_us;
 *   long            tmin_us, tmax_us, tavg_us;
 *   uint32_t        count;      (incremented before PROFILE_STOP()
 *                                 is called, since it divides by count)
 */
#define PROFILE_START()                                                \
	clock_gettime(CLOCK_MONOTONIC, &start)

#define PROFILE_STOP()                                                 \
	do {                                                            \
		/* time profiling */                                    \
		clock_gettime(CLOCK_MONOTONIC, &end);                   \
		time_us = (end.tv_sec - start.tv_sec) * 1000000 +       \
			(end.tv_nsec - start.tv_nsec) / 1000;           \
									 \
		if ((time_us < tmin_us) || (!tmin_us))                  \
			tmin_us = time_us;                              \
									 \
		if (time_us > tmax_us)                                  \
			tmax_us = time_us;                              \
									 \
		tavg_us = tavg_us + ((time_us - tavg_us) / count);      \
	} while (0)

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

/*
 * WAV "fmt " subchunk body (16 bytes, PCM). Packed because it mixes
 * 2-byte and 4-byte fields and must match the on-disk WAV layout
 * exactly, with no compiler-inserted padding.
 */
typedef struct __attribute__((packed)) {
	uint16_t       audio_format;     /* 1 = PCM */
	uint16_t       num_channels;
	uint32_t       sample_rate;
	uint32_t       byte_rate;
	uint16_t       block_align;
	uint16_t       bits_per_sample;
} fmt_chunk_t;

/*
 * Full WAV header capture writes for its own output file, built from
 * the actual capture format (SAMPLE_RATE/CHANNELS/16-bit) rather than
 * copied from any input file - capture has no input file at all.
 * data_hdr.size (and riff.size) are written as 0 placeholders up
 * front, since the total byte count isn't known until capture stops
 * (it runs until SIGINT), then patched via patch_wav_header().
 */
typedef struct __attribute__((packed)) {
	riff_header_t   riff;
	chunk_header_t  fmt_hdr;
	fmt_chunk_t     fmt;
	chunk_header_t  data_hdr;
} wav_header_t;

/*
 * Build and write a WAV header for capture's own output file, using
 * the actual capture format - not copied from any input file, since
 * capture has none. data/riff sizes are written as 0 placeholders
 * (the real total isn't known until capture stops); patch_wav_header()
 * fixes them up afterward.
 */
static int write_wav_header(FILE *fout, uint32_t sample_rate,
		uint16_t channels, uint16_t bits_per_sample)
{
	wav_header_t hdr = {
		.riff = {
			.id = { 'R', 'I', 'F', 'F' },
			.size = 0,
			.wave = { 'W', 'A', 'V', 'E' },
		},
		.fmt_hdr = {
			.id = { 'f', 'm', 't', ' ' },
			.size = sizeof(fmt_chunk_t),
		},
		.fmt = {
			.audio_format = 1, /* PCM */
			.num_channels = channels,
			.sample_rate = sample_rate,
			.byte_rate = sample_rate * channels *
					(bits_per_sample / 8),
			.block_align = (uint16_t)(channels *
					(bits_per_sample / 8)),
			.bits_per_sample = bits_per_sample,
		},
		.data_hdr = {
			.id = { 'd', 'a', 't', 'a' },
			.size = 0,
		},
	};

	if (fwrite(&hdr, sizeof(hdr), 1, fout) != 1) {
		printf("Error: Failed to write WAV header\n");
		return -1;
	}
	return 0;
}

/*
 * Patch the RIFF and data chunk sizes now that the real byte count is
 * known. Called once capture has stopped and the final total in
 * priv->out_bytes is final.
 */
static int patch_wav_header(FILE *fout, uint32_t data_bytes)
{
	uint32_t riff_size = (uint32_t)sizeof(wav_header_t) - 8 + data_bytes;

	if (fseek(fout, (long)offsetof(wav_header_t, riff.size), SEEK_SET) != 0) {
		printf("Error: Failed to seek to riff.size\n");
		return -1;
	}
	if (fwrite(&riff_size, sizeof(riff_size), 1, fout) != 1) {
		printf("Error: Failed to patch riff.size\n");
		return -1;
	}

	if (fseek(fout, (long)offsetof(wav_header_t, data_hdr.size), SEEK_SET) != 0) {
		printf("Error: Failed to seek to data_hdr.size\n");
		return -1;
	}
	if (fwrite(&data_bytes, sizeof(data_bytes), 1, fout) != 1) {
		printf("Error: Failed to patch data_hdr.size\n");
		return -1;
	}

	return 0;
}

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
	uint32_t        out_bytes;  /* capture: total bytes written to fout,
					used to patch the WAV header once
					capture stops (size isn't known
					up front - runs until SIGINT) */
	snd_pcm_t       *pcm;
	int             dbgfs_fd;
	pthread_mutex_t lock;
	bool            terminate;
	uint32_t        file_read_count;
	sem_t           start_pcm;  /* playback: chunk-ready signal, file->ring */
	sem_t           start_out;  /* capture:  chunk-ready signal, ring->file
					playback: one-shot gate for debugfs output thread */
} test_priv_t;

/*
 * Status codes for the FILE*-based ring buffer helpers.
 *
 * These are distinct from a plain buffer-full/buffer-empty condition:
 * a buffer-full/empty result is transient and expected under normal
 * xrun handling (retry, drop, or commit-zero as appropriate), while a
 * file I/O error means the underlying file itself is broken (bad
 * input data, disk full, etc). Collapsing both into a single bool
 * return makes an I/O error look like an ordinary xrun, which both
 * miscounts the xrun statistics and can hide a fatal, non-recoverable
 * condition behind what looks like routine buffer backpressure.
 */
typedef enum {
	BUF_IO_OK,
	BUF_IO_XRUN,   /* buffer full/empty: transient, expected, keep running */
	BUF_IO_ERR,    /* fread()/fwrite() failed: fatal, do not treat as xrun */
} buf_io_status_t;

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
static buf_io_status_t write_buf_from_file(file_rd_buf_t *buf, FILE *fin, size_t size)
{
	if ((buf->wr_id + 1) % AUDIO_BUF_LEN == buf->rd_id) {
		/* buffer full: transient, caller should retry */
		return BUF_IO_XRUN;
	}

	if (fread(&buf->buffer[buf->wr_id][0], 1, size, fin) != size) {
		/* fatal: real file read error, not a buffer condition */
		printf("Failed to read audio data\n");
		return BUF_IO_ERR;
	}
	buf->wr_id = (buf->wr_id + 1) % AUDIO_BUF_LEN;
	return BUF_IO_OK;
}

/* write audio data into circular buffer*/
static bool write_buf(file_rd_buf_t *buf, uint8_t *data)
{
	if ((buf->wr_id + 1) % AUDIO_BUF_LEN == buf->rd_id) {
		/* buffer full */
		return false;
	}

	memcpy(&buf->buffer[buf->wr_id][0], data, FRAME_BYTES);
	buf->wr_id = (buf->wr_id + 1) % AUDIO_BUF_LEN;
	return true;
}

/* read audio data from circular buffer, and write it into a file */
static buf_io_status_t read_buf_to_file(file_rd_buf_t *buf, FILE *fout, size_t size)
{
	if (buf->rd_id == buf->wr_id) {
		/* buffer empty: transient, expected underrun */
		return BUF_IO_XRUN;
	}

	if (fwrite(&buf->buffer[buf->rd_id][0], 1, size, fout) != size) {
		/* fatal: real file write error, not a buffer condition */
		printf("Failed to write out data\n");
		return BUF_IO_ERR;
	}
	buf->rd_id = (buf->rd_id + 1) % AUDIO_BUF_LEN;
	return BUF_IO_OK;
}
static buf_io_status_t read_buf_to_file_chunk(file_rd_buf_t *buf,FILE *fout,size_t size)
{
    uint32_t slots;
    uint32_t available;
    uint32_t first_slots;
    uint32_t second_slots;

    /* number of ring-buffer entries to consume */
    slots = size / FRAME_BYTES;

    /* determine available entries */
    if (buf->wr_id >= buf->rd_id)
        available = buf->wr_id - buf->rd_id;
    else
        available = AUDIO_BUF_LEN - buf->rd_id + buf->wr_id;

    /* not enough data available */
    if (available < slots)
        return BUF_IO_XRUN;

    /* contiguous entries before wrap */
    first_slots = AUDIO_BUF_LEN - buf->rd_id;
    if (first_slots > slots)
        first_slots = slots;

    /* write first contiguous block */
    if (fwrite(&buf->buffer[buf->rd_id][0],
               FRAME_BYTES,
               first_slots,
               fout) != first_slots) {
        printf("Failed to write out data\n");
        return BUF_IO_ERR;
    }

    /* write wrapped portion if needed */
    second_slots = slots - first_slots;

    if (second_slots) {
        if (fwrite(&buf->buffer[0][0],
                   FRAME_BYTES,
                   second_slots,
                   fout) != second_slots) {
            printf("Failed to write out data\n");
            return BUF_IO_ERR;
        }
    }

    /* consume all slots written */
    buf->rd_id = (buf->rd_id + slots) % AUDIO_BUF_LEN;

    return BUF_IO_OK;
}

/* input file read thread for playback test */
static void *playback_input_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	uint32_t remaining = priv->remaining;
	bool terminate = false;
	bool prebuffered = false;
	bool in_overrun = false;
	uint32_t count = 0;
	uint32_t sw_overrun = 0;   /* ring buffer full events (producer outran consumer) */
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	printf("Start %s\n", __func__);

	/* check for remaining data from input file */
	while (remaining > 0) {
		struct timespec start, end;
		long time_us;
		size_t this_block;

		PROFILE_START();

		/* check for terminate flag */
		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		this_block =
			(remaining > FRAME_BYTES) ? FRAME_BYTES : remaining;

		/* read data from file, and write it into circular buffer */
		switch (write_buf_from_file(&priv->buf, priv->fin, this_block)) {
		case BUF_IO_XRUN:
			/*
			 * Ring buffer full: wr_id would collide with rd_id.
			 * playback_pcm_thread isn't draining fast enough.
			 * Count this as one overrun episode (edge-triggered,
			 * not once per retry) and keep retrying: unlike
			 * capture, there's no hardware clock forcing us
			 * forward, so waiting for space is correct here
			 * rather than dropping audio data.
			 */
			if (!in_overrun) {
				in_overrun = true;
				sw_overrun++;
			}
			usleep(100);
			continue;
		case BUF_IO_ERR:
			/*
			 * Fatal: real file read error, not a buffer
			 * condition. Retrying would just spin forever
			 * since the read isn't going to fix itself, so
			 * stop this thread instead of miscounting it as
			 * an overrun.
			 */
			printf("ERROR: playback input file read failed,"
					" stopping (count: %u)\n", count);
			goto out;
		case BUF_IO_OK:
		default:
			break;
		}

		in_overrun = false;

		remaining -= (uint32_t)this_block;

		count++;

		pthread_mutex_lock(&priv->lock);
		priv->file_read_count = count;
		pthread_mutex_unlock(&priv->lock);

		PROFILE_STOP();

		if (!prebuffered && (count == CHUNK_SLOTS)) {
			/* first chunk (~900KB) buffered, safe to start playback */
			prebuffered = true;
			sem_post(&priv->start_pcm);
		}
	}

out:
	/*
	 * If we exit (EOF, terminate, or fatal read error) before ever
	 * reaching a full chunk (e.g. very short file), still wake the
	 * pcm thread so it doesn't block forever waiting on start_pcm.
	 */
	if (!prebuffered)
		sem_post(&priv->start_pcm);

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d, ring buffer"
			" overruns: %u\n",
			__func__, tmax_us, tmin_us, tavg_us, count, sw_overrun);

	//pthread_mutex_lock(&priv->lock);
	//priv->terminate = true;
	//pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/* PCM thread for playback test */
static void *playback_pcm_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	snd_pcm_t *pcm = priv->pcm;
	bool terminate = false;
	bool started = false;
	bool in_underrun = false;
	char *mmap_buf;
	uint32_t count = 0;
	uint32_t sw_underrun = 0; /* ring buffer empty events (consumer outran producer) */
	snd_pcm_uframes_t offset, mmap_frames = FRAME_SIZE;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;
	uint32_t file_read_count;

	/* wait until playback_input_thread has prebuffered one full chunk */
	sem_wait(&priv->start_pcm);
	printf("Start %s\n", __func__);

	pthread_mutex_lock(&priv->lock);
	file_read_count = priv->file_read_count;
	pthread_mutex_unlock(&priv->lock);

	while (count < file_read_count) {
		const snd_pcm_channel_area_t *areas;
		snd_pcm_sframes_t avail;
		int err;
		struct timespec start, end;
		long time_us;
		bool ret;

		PROFILE_START();

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
			/*
			 * Ring buffer empty: rd_id caught up to wr_id.
			 * playback_input_thread isn't filling fast enough
			 * relative to ALSA's consumption rate. There is no
			 * new audio to give the hardware this period, so we
			 * commit 0 frames (nothing to drop - there's nothing
			 * there) and let ALSA either wait or, if this starves
			 * it long enough, report a real hw xrun, which is a
			 * fatal error above (exits the application).
			 */
			if (!in_underrun) {
				in_underrun = true;
				sw_underrun++;
			}
			snd_pcm_mmap_commit(pcm, offset, 0);
			continue;
		}
		in_underrun = false;

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

		pthread_mutex_lock(&priv->lock);
		file_read_count = priv->file_read_count;
		pthread_mutex_unlock(&priv->lock);

		PROFILE_STOP();

		if (count == 1) {
			/* signal out thread */
			sem_post(&priv->start_out);
		}
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d, ring buffer"
			" underruns: %u\n",
			__func__, tmax_us, tmin_us, tavg_us, count,
			sw_underrun);

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

		PROFILE_START();

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

		PROFILE_STOP();
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
	sem_destroy(&priv->start_pcm);
	sem_destroy(&priv->start_out);
	pthread_mutex_destroy(&priv->lock);
	free(priv);
	return 0;
}

/*
 * PCM capture thread ("capture_playback_thread"): reads live audio from
 * the ALSA capture (mic) interface via mmap and pushes it into the ring
 * buffer. No input.wav is used for capture - this thread is the sole
 * producer, driven purely by the sound interface, and runs until the
 * user stops the app (Ctrl+C / SIGINT) or an ALSA error occurs.
 */
static void *capture_playback_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	snd_pcm_t *pcm = priv->pcm;
	bool terminate = false;
	bool started = false;
	bool prebuffered = false;
	bool in_overrun = false;
	char *mmap_buf;
	uint32_t count = 0;
	uint32_t chunk_count = 0;
	uint32_t sw_overrun = 0; /* ring buffer full events (consumer outran producer) */
	snd_pcm_uframes_t offset, mmap_frames = FRAME_SIZE;
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	printf("Start %s\n", __func__);

	while (true) {
		const snd_pcm_channel_area_t *areas;
		snd_pcm_sframes_t avail;
		int err;
		struct timespec start, end;
		long time_us;
		bool ret;

		PROFILE_START();

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
			/*
			 * Ring buffer full: wr_id would collide with rd_id.
			 * capture_output_thread isn't draining fast enough.
			 * Drop this period's captured audio (it's lost -
			 * there was nowhere to put it) but still commit the
			 * full mmap_frames so the ALSA hw pointer keeps
			 * advancing; committing 0 here would stall hw
			 * consumption and turn a software overrun into a
			 * real hardware overrun too.
			 */
			if (!in_overrun) {
				in_overrun = true;
				sw_overrun++;
			}
			snd_pcm_mmap_commit(pcm, offset, mmap_frames);
			continue;
		}
		in_overrun = false;

		err = snd_pcm_mmap_commit(pcm, offset, mmap_frames);
		if (err < 0) {
			printf("%s: snd_pcm_mmap_commit failed (count: %u)\n",
					__func__, count);

			/* Exit from application */
			break;
		}

		count++;

		PROFILE_STOP();

		chunk_count++;
		if (chunk_count == CHUNK_SLOTS) {
			/* a full ~920KB chunk is ready: wake output thread */
			chunk_count = 0;
			prebuffered = true;
			sem_post(&priv->start_out);
		}
	}

	/*
	 * Flush whatever is left (partial final chunk, or nothing ever
	 * reached a full chunk) so capture_output_thread wakes up, drains
	 * the remainder, and observes the terminate flag instead of
	 * blocking forever.
	 */
	if (!prebuffered || chunk_count > 0)
		sem_post(&priv->start_out);

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d, ring buffer"
			" overruns: %u\n",
			__func__, tmax_us, tmin_us, tavg_us, count,
			sw_overrun);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/*
 * Output file write thread for capture test: consumes the ring buffer
 * filled by capture_playback_thread and writes it to the output WAV
 * file. Instead of busy-polling, it blocks on priv->start_out and wakes
 * only when a ~900KB chunk (or the final partial chunk) is ready, then
 * drains that whole chunk in one batch. This is the "granular buffer"
 * signal: chunk fills -> signal -> consumer processes that chunk.
 */
static void *capture_output_thread(void *arg)
{
	test_priv_t *priv = (test_priv_t *)arg;
	bool terminate = false;
	bool in_underrun = false;
	uint32_t count = 0;
	uint32_t sw_underrun = 0; /* buffer empty when a chunk was signalled */
	long tmin_us = 0, tmax_us = 0, tavg_us = 0;

	printf("Start %s\n", __func__);

	while (true) {
		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		/* block here until a chunk is ready (or thread is told to exit) */
		sem_wait(&priv->start_out);

		struct timespec start, end;
		long time_us;

		pthread_mutex_lock(&priv->lock);
		terminate = priv->terminate;
		pthread_mutex_unlock(&priv->lock);
		if (terminate)
			break;

		PROFILE_START();

		/* read audio data from circular buffer, write it to output file */
		switch (read_buf_to_file_chunk(&priv->buf, priv->fout, CHUNK_SLOTS*FRAME_BYTES)) {
			case BUF_IO_XRUN:
				/*
				 * A chunk was signalled but rd_id caught up
				 * to wr_id already: producer isn't filling
				 * fast enough. Count this as one underrun
				 * episode (edge-triggered, not once per
				 * retry) and keep running - the next signal
				 * will bring more data.
				 */
				if (!in_underrun) {
					in_underrun = true;
					sw_underrun++;
				}
				continue;
			case BUF_IO_ERR:
				/*
				 * Fatal: real file write error (disk full,
				 * output closed, etc), not a buffer
				 * condition. Don't miscount it as an
				 * underrun and don't keep silently losing
				 * captured audio to a broken output file -
				 * stop the whole test.
				 */
				printf("ERROR: capture output file write"
						" failed, stopping"
						" (count: %u)\n", count);
				pthread_mutex_lock(&priv->lock);
				priv->terminate = true;
				pthread_mutex_unlock(&priv->lock);
				goto next_chunk;
			case BUF_IO_OK:
			default:
				break;
		}
		in_underrun = false;

		count++;
		//priv->out_bytes += FRAME_BYTES;
		priv->out_bytes += FRAME_BYTES * CHUNK_SLOTS;

		PROFILE_STOP();
		continue;
next_chunk:
		break;
	}

	printf("Exit from %s: max time: %ld us, min time: %ld us,"
			" avg time: %ld us, total count: %d, ring buffer"
			" underruns: %u\n",
			__func__, tmax_us, tmin_us, tavg_us, count, sw_underrun);

	pthread_mutex_lock(&priv->lock);
	priv->terminate = true;
	pthread_mutex_unlock(&priv->lock);

	return NULL;
}

/*
 * Capture test: no input.wav is read or written to any debugfs node.
 * Only two threads run:
 *   - capture_playback_thread : mic (ALSA) -> ring buffer
 *   - capture_output_thread   : ring buffer -> output wav file
 * Runs continuously (long-run) until Ctrl+C (SIGINT).
 */
static int test_capture(FILE *fout)
{
	snd_pcm_t *pcm = NULL;
	snd_pcm_hw_params_t *hw = NULL;
	snd_pcm_uframes_t period_size, buffer_size;
	int err;
	test_priv_t *priv;
	pthread_t cap_playback_t, cap_out_t;

	priv = malloc(sizeof(test_priv_t));
	if(!priv) {
		printf("ERROR: malloc failed for test_priv_t\n");
		return -ENOMEM;
	}
	priv->fin = NULL;
	priv->fout = fout;
	priv->remaining = 0;
	priv->out_bytes = 0;
	priv->dbgfs_fd = -1;

	/* global variable to use in signal handler */
	g_priv = priv;

	/* open PCM channel for capture */
	err = snd_pcm_open(&pcm, PCM_DEV, SND_PCM_STREAM_CAPTURE, 0);
	if (err < 0) {
		printf("ERROR: cannot open PCM: %s\n", snd_strerror(err));
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

	if (pthread_create(&cap_playback_t, NULL, capture_playback_thread, priv)) {
		printf("pthread_create for capture_playback_thread failed\n");
		goto cleanup;
	}

	if (pthread_create(&cap_out_t, NULL, capture_output_thread, priv)) {
		printf("pthread_create for capture_output_thread failed\n");

		pthread_mutex_lock(&priv->lock);
		priv->terminate = true;
		pthread_mutex_unlock(&priv->lock);

		pthread_join(cap_playback_t, NULL);
		goto cleanup;
	}

	printf("%s: App started. Press CTRL+C to Stop Application.\n", __func__);

	pthread_join(cap_playback_t, NULL);
	pthread_join(cap_out_t, NULL);

	/*
	 * Capture ran until SIGINT, so the total size wasn't known when
	 * write_wav_header() wrote the placeholder sizes - fix them up
	 * now with the real byte count.
	 */
	if (patch_wav_header(priv->fout, priv->out_bytes))
		printf("WARN: failed to patch WAV header with final size\n");

cleanup:
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
	sem_destroy(&priv->start_pcm);
	sem_destroy(&priv->start_out);
	pthread_mutex_destroy(&priv->lock);
	free(priv);
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

	if (argc < 2) {
		printf("Usage: %s playback <input.wav> <output.wav>\n"
				"       %s capture <output.wav>\n",
				argv[0], argv[0]);
		return ret;
	}

	if (!strcmp(argv[1], "capture")) {
		if (argc != 3) {
			printf("Usage: %s capture <output.wav>\n", argv[0]);
			return ret;
		}

		/* capture has no input file at all - open only the output */
		fout = fopen(argv[2], "wb");
		if (!fout) {
			printf("Failed to open %s\n", argv[2]);
			return ret;
		}

		/*
		 * Generate the output WAV header from the actual capture
		 * format (SAMPLE_RATE/CHANNELS/16-bit) instead of copying
		 * one from an input file - capture has none to copy from.
		 */
		if (write_wav_header(fout, SAMPLE_RATE, CHANNELS, 16)) {
			fclose(fout);
			return ret;
		}

		ret = test_capture(fout);
		fclose(fout);
		return ret;
	}

	if (strcmp(argv[1], "playback")) {
		printf("Invalid option\n");
		return ret;
	}

	if (argc != 4) {
		printf("Usage: %s playback <input.wav> <output.wav>\n",
				argv[0]);
		return ret;
	}

	/* open input file for read */
	fin = fopen(argv[2], "rb");
	if (!fin) {
		printf("Failed to open %s\n", argv[2]);
		return ret;
	}

	/* open output file for write */
	fout = fopen(argv[3], "wb");
	if (!fout) {
		printf("Failed to open %s\n", argv[3]);
		fclose(fin);
		return ret;
	}

	/* parse header of input file and copy this to output file */
	if (parse_header(fin, fout, &data_size)) {
		printf("Failed to parse header\n");
		fclose(fin);
		fclose(fout);
		return ret;
	}

	ret = test_playback(fin, fout, data_size);

	fclose(fin);
	fclose(fout);
	return ret;
}