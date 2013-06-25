
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include "mytcp.h"

void *connect_to_host(uint32_t ip, uint16_t port) {
	struct sockaddr_in sockaddr_server;
	int socket_descriptor;
	int ret;

	socket_descriptor = socket (AF_INET, SOCK_STREAM, 0);

	bzero (&sockaddr_server, sizeof(sockaddr_server));
	sockaddr_server.sin_family = AF_INET;
	sockaddr_server.sin_addr.s_addr = ip;
	sockaddr_server.sin_port = htons(port);

	ret = connect(socket_descriptor, (struct sockaddr*)&sockaddr_server, sizeof(sockaddr_server));
	if (ret != 0) {
		printf ("Connection to target failed in function connect_to_host()\n");
		return NULL;
	}
	printf ("Connection to target succeded\n");

	return (void *)socket_descriptor;
}

int create_listen_socket(int port) {
	int server_socket_descriptor;
	struct sockaddr_in sockaddr_server;

	if ((server_socket_descriptor = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		fprintf (stderr, "Could not create socket.\n");
		return -1;
	}

	bzero(&sockaddr_server, sizeof(sockaddr_server));
	sockaddr_server.sin_family = AF_INET;
	sockaddr_server.sin_addr.s_addr = htonl(INADDR_ANY);
	sockaddr_server.sin_port = htons(port);

	if (bind(server_socket_descriptor, (struct sockaddr *) &sockaddr_server, sizeof(sockaddr_server)) < 0) {
		fprintf (stderr, "Bind error.\n");
		return -1;
	}

	if (listen(server_socket_descriptor, MAXPENDING) < 0) {
		fprintf (stderr, "Listen error.\n");
		return -1;
	}
	return server_socket_descriptor;
}


