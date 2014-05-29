#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include "serial.h"

#define PORT "6667"
#define MAXMSG 100
#define BACKLOG 5



int main(void) {

	struct addrinfo hints;
	struct addrinfo *serverInfo;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hints.ai_flags = AI_PASSIVE;// Asigna el address del localhost: 127.0.0.1
	hints.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP

	getaddrinfo(NULL, PORT, &hints, &serverInfo);
	int listenningSocket;
	listenningSocket = socket(serverInfo->ai_family, serverInfo->ai_socktype,
			serverInfo->ai_protocol);
	bind(listenningSocket, serverInfo->ai_addr, serverInfo->ai_addrlen);
	freeaddrinfo(serverInfo);

	fd_set active_fd_set, read_fd_set;
	int i;
	struct sockaddr_in clientname;
	socklen_t size;

	listen(listenningSocket, BACKLOG);

	FD_ZERO(&active_fd_set);
	FD_SET(listenningSocket, &active_fd_set);

	while (1) {
		read_fd_set = active_fd_set;
		if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL ) < 0) {
			perror("select");
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < FD_SETSIZE; ++i) // FD_SETSIZE vale 1024 por defecto
			if (FD_ISSET (i, &read_fd_set)) { //FD_ISSET Retorna True si i (socket) esta en el set.
				if (i == listenningSocket) { //Compara si i es el socket por el que estaba escuchando
					/* Connection request on original socket. */
					int new;
					size = sizeof(clientname);
					new = accept(listenningSocket,
							(struct sockaddr *) &clientname, &size);
					if (new < 0) {
						perror("accept");
						exit(EXIT_FAILURE);
					}
					printf("Un Cliente se acaba de conectar, port %d.\n",
							ntohs(clientname.sin_port));
					FD_SET(new, &active_fd_set);
				} else {
					char package[MAXMSG];
					cabecera_t cabecera;

					int status = recv(i, &cabecera, sizeof(cabecera), 0);


					if (status != 0) {
						//printf("%d", );
						switch (cabecera.identificador) {
											case IdHandshake: {
												idproceso_t idProceso; /* Se declara una variable del mensaje que queremos leer */
												recv(i, &idProceso, sizeof(idProceso), 0); /* Se lee */
												printf("Mensaje recibido: %s", idProceso.tipo);
												break;
											}
											case IdProgramaActivo: {
												break;
											}
											}
					} else {
						printf("Se cerro la conexion \n");
						close(i);
						FD_CLR(i, &active_fd_set);
					}
				}
			}
	}
}
