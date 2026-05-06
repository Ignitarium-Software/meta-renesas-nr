#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define PORT 8080

#define AWE_MAX_PKT_LEN (1056)
uint8_t ip_buffer[AWE_MAX_PKT_LEN];
uint8_t resp_buffer[AWE_MAX_PKT_LEN];

static void init_buffer(void)
{
    for (int i = 0; i < AWE_MAX_PKT_LEN; i++) {
        ip_buffer[i] = i%128;
    }
}

int main(int argc, char const* argv[])
{
	init_buffer();
#if 0
    int server_fd, new_socket;
    ssize_t bytes_read;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // Forcefully attaching socket to the port 8080
    if (setsockopt(server_fd, SOL_SOCKET,
                   SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    /* Forcefully attaching socket to the port 8080 */
    if (bind(server_fd, (struct sockaddr*)&address,
             sizeof(address))
        < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    if ((new_socket
         = accept(server_fd, (struct sockaddr*)&address,
                  &addrlen))
        < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }
#else

	#define PORT 8080
	#define SERVER_IP "192.168.0.120"

    ssize_t bytes_read;
	int status, new_socket;
    struct sockaddr_in serv_addr;
    if ((new_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    /* Convert IPv4 and IPv6 addresses from text to binary form */
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        printf("Invalid address/ Address not supported \n");
        return -1;
    }

    if ((status = connect(new_socket, (struct sockaddr*)&serv_addr,
                   sizeof(serv_addr))) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    printf("Connected to server\n");


#endif

	const int len[] = {127, 255, 511, 767, 1056};
	int loop = 0;
  
	do {
		if (loop > 4)
		{
			loop = 0;
		}

		printf("Length : %d \n", len[loop]);
    	send(new_socket, ip_buffer, len[loop++], 0);

    	bytes_read = read(new_socket, resp_buffer, AWE_MAX_PKT_LEN);

#if 1
		if (memcmp(resp_buffer, ip_buffer, bytes_read) != 0) {
			printf("Mem comparison failed\n");
		}
		else
		{
			printf("recevied buffer is matching\n");
		}
#else
		for(int i = 0; i < bytes_read; i++)
		{
			if (resp_buffer[i] != ip_buffer[i])
			{
				printf("Incorrect value at : %d, value : %x\n", i, ip_buffer[i]);
			}
		}

#endif
		usleep(5000000);

	} while(1);

    /* closing the connected socket */
    close(new_socket);
  
    /* closing the listening socket */
    //close(server_fd);
    return 0;
}
