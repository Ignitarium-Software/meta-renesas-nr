#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define AWE_TUNING_PORT 15004

int reset_tuning_socket(int client_socket, int sockfd)
{
    int newsockfd;
    socklen_t clilen;
    struct sockaddr_in cli_addr;
    shutdown(client_socket, SHUT_RDWR);
    close(client_socket);

    listen(sockfd, 3);
    printf("Listening again for connection on port %d\n", AWE_TUNING_PORT);
    clilen = sizeof(cli_addr);
    newsockfd = accept(sockfd,
                 (struct sockaddr *) &cli_addr,
                 &clilen);

    if (newsockfd < 0)
    {
        printf("ERROR on accept, %d\n", newsockfd);
        return -1;
    }
    printf( "Found connection again!\n");
    return newsockfd;
}

int init_tcp_server(int *server_fd)
{
	int new_socket, opt = 1;
    struct sockaddr_in address = {0};
	char client_ip[INET_ADDRSTRLEN];
    socklen_t addrlen = sizeof(address);

    /* Creating socket file descriptor */
    if ((*server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
		return -1;
    }

    /* Forcefully attaching socket to the port */
    if (setsockopt(*server_fd, SOL_SOCKET,
                   SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(opt))) {
        perror("setsockopt");
		return -1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(AWE_TUNING_PORT);

    /* Forcefully attaching socket to the port */
    if (bind(*server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
		return -1;
    }

    if (listen(*server_fd, 3) < 0) {
        perror("listen");
		return -1;
    }

	printf("Start the client application...\n");

    if ((new_socket = accept(*server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
        perror("accept");
		return -1;
    }

    inet_ntop(AF_INET, &address.sin_addr, client_ip, INET_ADDRSTRLEN);

    printf("Client connected, IP: %s\n", client_ip);

	return new_socket;
}
