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
#include "tuning_server.h"

#define CTRL_DEV "/dev/rpmsg_ctrl0"
#define RPMSG_SERVICE_NAME "rpmsg-client-sample"

static volatile sig_atomic_t stop_flag = 0;

uint8_t ip_buffer[AWE_MAX_PKT_LEN];
uint8_t resp_buffer[RPMSG_PKT_LEN];
int tun_ept_fd;
sem_t awe_sem;

int init_tcp_server(int *server_fd);
int get_endpoint_info(int *src_addr, int *dst_addr);

static void sig_handler(int signo)
{
	printf("Recevied ^C interrupt\n");
	stop_flag = 1;
}

/**
 * @brief Receive the incoming rp message data from the R core
 * and signal the semaphore
 */
void* awe_resp_thread(void* arg)
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

	/* open control endpoint */
	ctrl_fd = open(CTRL_DEV, O_RDWR);
	if (ctrl_fd < 0) {
		perror("open ctrl device");
		return -1;
	}

	if (get_endpoint_info(&ep_src, &ep_dst) != 0) {
		printf("Failed to get endpoint info\n");
		close(ctrl_fd);
		return -1;
	}

	memset(&eptinfo, 0, sizeof(eptinfo));
	strncpy(eptinfo.name, RPMSG_SERVICE_NAME, sizeof(eptinfo.name));
	eptinfo.src = ep_src;
	eptinfo.dst = ep_dst;

	ret = ioctl(ctrl_fd, RPMSG_CREATE_EPT_IOCTL, &eptinfo);
	if (ret < 0) {
		perror("ioctl create endpoint");
		close(ctrl_fd);
		return -1;
	}

	snprintf(dev_name, sizeof(dev_name), "/dev/rpmsg%d", ret);

	printf("Endpoint Info :%x, %x\n", eptinfo.dst, eptinfo.src);

	tun_ept_fd = open(dev_name, O_RDWR);
	if (tun_ept_fd < 0) {
		perror("open endpoint device");
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
		int buflen = read(client_socket, ip_buffer, AWE_MAX_PKT_LEN);
		if (buflen <= 0 ) {
			perror("failed to read data");
			break;
		}
		
		/* send data to remote core */
		if (send_awe_pkts_fully(tun_ept_fd, ip_buffer, buflen) != 0)
		{
			printf("Failed to send data\n");
			break;
		}

		/* wait for response from the remote core */
		sem_wait(&awe_sem);

		/* send response back to tcp server */
		buflen = send_response(client_socket);
		if (buflen <= 0) {
			perror("failed to send data to server");
			break;
		}

	} while (stop_flag == 0);

	/* Close all file descriptor */
	close(tun_ept_fd);
	close(ctrl_fd);
	close(client_socket);
	close(server_fd);

	printf("Exiting the application\n");

	return 0;
}
