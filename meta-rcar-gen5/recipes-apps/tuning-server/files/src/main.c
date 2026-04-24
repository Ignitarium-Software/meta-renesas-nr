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

int init_tcp_client(void);
int get_endpoint_info(int *dst_addr);

static void on_sigint(int signo)
{
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
                printf("Failed to receive message");
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
	int ret, ep_dst, client_fd, ctrl_fd;
	pthread_t awe_resp_tid;

	/* initialize semaphore */
	sem_init(&awe_sem, 0, 0);

    if (signal(SIGINT, on_sigint) == SIG_ERR) {
        printf("Failed to set sigint\n");
        return -1;
    }

	client_fd = init_tcp_client();
    if (client_fd < 0 ) {
        printf("Failed to setup socket\n");
        return -1;
    }

	/* open control endpoint */
    ctrl_fd = open(CTRL_DEV, O_RDWR);
    if (ctrl_fd < 0) {
        perror("open ctrl device");
        return -1;
    }

	if (get_endpoint_info(&ep_dst) != 0) {
		printf("Failed to get endpoint info\n");
		close(ctrl_fd);
		return -1;
	}

    memset(&eptinfo, 0, sizeof(eptinfo));
    strncpy(eptinfo.name, RPMSG_SERVICE_NAME, sizeof(eptinfo.name));
    eptinfo.src = 1024;
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
		int buflen = read(client_fd, ip_buffer, AWE_MAX_PKT_LEN);
		if (buflen <= 0 ) {
			perror("failed to read data\n");
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
		buflen = send_response(client_fd);
		if (buflen <= 0) {
			perror("failed to send data to server");
			break;
		}

	} while (stop_flag == 0);

	stop_flag = 0;

	printf("\nExiting the application");
    close(tun_ept_fd);
    close(ctrl_fd);

    return 0;
}
