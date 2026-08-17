#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/rpmsg.h>
#include <signal.h>
#include <semaphore.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include "tuning_server.h"

/* tuning endpoint service name */
#define RPMSG_SERVICE_NAME "tuning_ep"

static volatile sig_atomic_t stop_flag = 0;

uint32_t ip_buffer[AWE_PACKET_MAX_WORDS];
uint8_t resp_buffer[RPMSG_PKT_LEN];
int tun_ept_fd;
sem_t awe_sem;

static void sig_handler(int signo)
{
	printf("Recevied ^C interrupt\n");
	stop_flag = 1;
}

/**
 * @brief Receive the incoming rp message data from the DSP core
 * and signal the semaphore
 */
static void* awe_resp_thread(void* arg)
{
	while (stop_flag == 0) {
		do {
			int rlen = read(tun_ept_fd, resp_buffer, RPMSG_PKT_LEN);
		  	if (rlen <= 0) {
				perror("Failed to receive message");
				break;
			}
			else
			{
				if (aggregate_awe_pkts(resp_buffer, rlen) == true)
				{
					/* signal that response was received completely */
					sem_post(&awe_sem);
				}
				break;
			}
		} while (1);

		usleep(100);
	}
	return NULL;
}

int main()
{
	struct rpmsg_endpoint_info eptinfo;
	char dev_name[32];
	int ret, ep_src, ep_dst, client_socket, ctrl_fd, server_fd;
	int virtio_id;
	char ctrl_device[128];
	pthread_t awe_resp_tid;

	/* initialize semaphore */
	sem_init(&awe_sem, 0, 0);

	struct sigaction sa = {0};

	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	/* IMPORTANT: no SA_RESTART */
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("SIGINT sigaction");
		return -1;
	}

	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		perror("SIGTERM sigaction");
		return -1;
	}

	printf("Initializing the tcp server\n");

	client_socket = init_tcp_server(&server_fd);
	if (client_socket < 0 ) {
		printf("Failed to setup socket\n");
		return -1;
	}

	if (get_endpoint_info(&ep_src, &ep_dst, &virtio_id) != 0) {
		printf("Failed to get endpoint info\n");
		close(ctrl_fd);
		return -1;
	}

	snprintf(ctrl_device, sizeof(ctrl_device), "%s%d","/dev/rpmsg_ctrl", virtio_id);

	printf("Control Endpoint: %s\n", ctrl_device);

	/* open control endpoint */
	ctrl_fd = open(ctrl_device, O_RDWR);
	if (ctrl_fd < 0) {
		printf("Failed to open rpmsg control node %d\n", errno);
		return -1;
	}

	memset(&eptinfo, 0, sizeof(eptinfo));
	strncpy(eptinfo.name, RPMSG_SERVICE_NAME, sizeof(eptinfo.name));
	eptinfo.src = ep_src;
	eptinfo.dst = ep_dst;

	ret = ioctl(ctrl_fd, RPMSG_CREATE_EPT_IOCTL, &eptinfo);
	if (ret < 0) {
		printf("ioctl endpoint creation failed %d\n", errno);
		close(ctrl_fd);
		return -1;
	}

	snprintf(dev_name, sizeof(dev_name), "/dev/rpmsg%d", ret);

	printf("Endpoint Info :%x, %x\n", eptinfo.dst, eptinfo.src);

	tun_ept_fd = open(dev_name, O_RDWR);
	if (tun_ept_fd < 0) {
		printf("rpmsg endpoint openeing failed %d", errno);
		close(ctrl_fd);
		return -1;
	}

	if (pthread_create(&awe_resp_tid, NULL, awe_resp_thread, NULL) < 0) {
		perror("Unable to create thread");
		close(tun_ept_fd);
		close(ctrl_fd);
		return -1;
	}

	do {
		int read_bytes = 0;
		uint32_t pkt_len;

		read_bytes = read(client_socket, ip_buffer, AWE_MAX_PKT_LEN);
		if (read_bytes <= 0 ) {
			printf("ERROR: socket read error, attempt to reconnect --  ret = %d, errno = %d\n", read_bytes, errno);
			client_socket = reset_tuning_socket(client_socket, server_fd);
			if (client_socket < 0) {
				printf("Invalid socket descriptor\n");
				break;
			}
			continue;
		}

		pkt_len = PACKET_LENGTH_BYTES(ip_buffer);

		while (read_bytes < pkt_len) {
			printf("Didn't read the entire packet! readBytes = %d, totalPacketLength = %u\nReading again\n", read_bytes, pkt_len);
			read_bytes += read(client_socket, &((char *)ip_buffer)[read_bytes], AWE_MAX_PKT_LEN);
		}

		if (read_bytes > pkt_len) {
			printf("ERROR: read %d bytes, expected maximum is %u. Exiting.\n", read_bytes, pkt_len);
			client_socket = reset_tuning_socket(client_socket, server_fd);
			if (client_socket < 0) {
				printf("Invalid socket descriptor\n");
				break;
			}
			continue;
		}

#ifdef DEBUG_PRINT
		uint32_t pkt_len_words = PACKET_LENGTH_WORDS(ip_buffer);
		printf("No. of packets : %d\n", pkt_len_words);

		for (int i = 0; i < pkt_len_words; i++)
			printf("Packet %dth, %x ",i, ip_buffer[i]);
		printf("\n");
#endif

		/* send data to remote core */
		ret = send_awe_pkts_fully(tun_ept_fd, (uint8_t *)ip_buffer, read_bytes);
		if (ret != 0) {
			printf("Failed to send data to DSP core, %d\n", ret);
			break;
		}

		/* wait for response from the remote core */
		sem_wait(&awe_sem);

#ifdef DEBUG_PRINT
		uint32_t *buf = (uint32_t *)&resp_buffer[0];
		pkt_len_words = PACKET_LENGTH_WORDS(buf);
		printf("Response received from DSP");
		for (int i = 0; i < pkt_len_words; i++)
			printf("%x ", buf[i]);
		printf("\n");
#endif

		/* send response back to tcp server */
		read_bytes = send_response(client_socket);
		if (read_bytes <= 0) {
			printf("failed to send data to client %d\n", errno);
			client_socket = reset_tuning_socket(client_socket, server_fd);
			if (client_socket < 0) {
				printf("Invalid socket descriptor\n");
				break;
			}
			continue;
		}

	} while (stop_flag == 0);

	ret = ioctl(tun_ept_fd, RPMSG_DESTROY_EPT_IOCTL, &eptinfo);
	if (ret < 0) {
		printf("Failed to release rpmsg endpoint %d\n", errno);
	}
    else {
        printf("Endpoint released\n");
    }

	/* Close all file descriptor */
	close(tun_ept_fd);
	close(ctrl_fd);
	close(client_socket);
	close(server_fd);

	printf("Exiting the application\n");

	return 0;
}
