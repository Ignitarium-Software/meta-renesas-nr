#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PORT 8080
#define SERVER_IP "192.168.0.20"

int init_tcp_server(void)
{
	int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    /* Creating socket file descriptor */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
		return -1;
    }

    /* Forcefully attaching socket to the port */
    if (setsockopt(server_fd, SOL_SOCKET,
                   SO_REUSEADDR | SO_REUSEPORT, &opt,
                   sizeof(opt))) {
        perror("setsockopt");
		return -1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    /* Forcefully attaching socket to the port */
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
		return -1;
    }
    if (listen(server_fd, 3) < 0) {
        perror("listen");
		return -1;
    }
    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
        perror("accept");
		return -1;
    }

	printf("Connected to client\n");

	char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address.sin_addr,
              client_ip, INET_ADDRSTRLEN);

    printf("Client connected!\n");
    printf("IP: %s\n", client_ip);
    printf("Port: %d\n", ntohs(address.sin_port));

	return new_socket;
}
