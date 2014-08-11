/*
 ============================================================================
 Name        : ProcesoUMV.c
 Author      : Andrés Guibarra
 Version     :
 Copyright   : Your copyright notice
 Description : Proceso UMV Encargado de la administración de memoria del Intérprete de AnsisOp
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <commons/string.h>
#include <commons/bitarray.h>
#include <commons/config.h>
#include <commons/error.h>
#include <commons/log.h>
#include <pthread.h>
#include "serial.h"
#include "ProcesoUMV.h"

#define BACKLOG 5			// Define cuantas conexiones vamos a mantener pendientes al mismo tiempo
#define PACKAGESIZE 1024

void obtenerValoresConfiguracion(char *VARIABLE, char *expected, int tamanio, t_config *config);
void iniciarUMV(char **cadena);
void *recibirPorSockets();
void *conexionCliente(void *param);
void *operacionesConsola();
void destruirSegmentos(destruirsegmento_t destruir_segmentos);
void calcularTamanioDisponible(int *posicion, int *mayorEspacio, int tamanio);
void calcularTamanio_WORST_FIT(int *posicion, int *mayorEspacio);
int crearSegmento(crearsegmento_t segmento);
int alojarSegmentos();
int almacenarBytes(bytes_t bytes, void *buffer);
int solicitarBytes(bytes_t bytes, void *buffer);
int buscarDireccion(int base, int *tamanio);
void compactacion();
int retardoGlobal = 0;
int auxTamanioBloqueMemoria;
char *algoritmo, *IP, *PUERTO, *CONSOLA_ACTIVA, *LOG_LEVEL;
void* mallocInit;
int programaActivo;
segmentos_memoria_t *segmentosMemoria;
pthread_mutex_t crearSegmentMutex;
pthread_mutex_t destruirSegmentoMutex;
pthread_mutex_t enviarBytesMutex;
pthread_mutex_t compactacionMutex;
t_log *logger;
char *log_file_name = "log_umv";
void obtenerValoresConfiguracion(char *VARIABLE, char *expected, int tamanio, t_config *config) {
	if (config_has_property(config, expected)) {

		memcpy(VARIABLE, config_get_string_value(config, expected), tamanio);

	} else {
		error_show("Se necesita %s de %s \n", string_split(expected, "_")[0], string_split(expected, "_")[1]);
	}
}

int main(int argc, char **argv) {
	srand(getpid() * time(NULL )); //Inicializa la semilla para los numeros Random
	iniciarUMV(argv);
	pthread_mutex_init(&crearSegmentMutex, NULL );
	pthread_mutex_init(&destruirSegmentoMutex, NULL );
	pthread_mutex_init(&enviarBytesMutex, NULL );
	pthread_mutex_init(&compactacionMutex, NULL );
	pthread_t hiloConsola;
	pthread_t hiloSockets;
	pthread_create(&hiloConsola, NULL, operacionesConsola, NULL );
	pthread_create(&hiloSockets, NULL, recibirPorSockets, NULL );
	pthread_join(hiloConsola, NULL );
	pthread_join(hiloSockets, NULL );
	return 0;
}

void* operacionesConsola() {

	char **token;
	char comando[100];
	char *ingreso = comando;
	system("clear");
	printf("UMV>");
	while (1) {

		gets(ingreso);

		if (string_length(ingreso) != 0) {

			token = string_split(ingreso, " ");
			if (string_equals_ignore_case(token[0], "operacion")) {
				t_log *opcional = log_create("Buffer", "UMV", false, LOG_LEVEL_INFO);
				int idPCB, base, offset, tamanio;
				if (!token[5]) {
					idPCB = atoi(token[1]);
					base = atoi(token[2]);
					offset = atoi(token[3]);
					tamanio = atoi(token[4]);
					printf(" 1 Solicitar bytes \n 2 Escribir en buffer \n 3 Crear Segmento  \n 4 Destruir segmento de memoria \n");
					printf("Ingrese una opcion:");
					int opcion;
					scanf("%d", &opcion);
					switch (opcion) {
						case 1: {
							bytes_t bytes;
							bytes.base = base;
							bytes.offset = offset;
							bytes.tamanio = tamanio;
							void *buffer = malloc(bytes.tamanio);
							if (solicitarBytes(bytes, buffer) != -1) {

								printf("Lo que hay es: %s \n", (char *) buffer);
								printf("Guardar en Archivo?[s/n]\n");
								char *opFile = malloc(2);
								scanf("%s", opFile);
								if (string_equals_ignore_case(opFile, "s")) {
									printf("Guardado\n");
									log_info(opcional, "%s", (char *) buffer);
								}
								free(opFile);
							} else {
								error_show("Segmentation Fault");
								log_error(logger, "Segmentation Fault");
							}
							//free(buffer);
							break;
						}
						case 2: {
							char str[tamanio];
							printf("Ingrese el texto a guardar en Memoria:");
							while (getchar() != '\n')
								;
							fgets(str, tamanio, stdin);
							str[tamanio] = '\0';
							bytes_t bytes;
							bytes.base = base;
							bytes.tamanio = tamanio;
							bytes.offset = offset;

							if (almacenarBytes(bytes, &str) == -1) {
								error_show("Segmentation Fault");
								log_error(logger, "Segmentation Fault");
							}

							break;
						}
						case 3: {
							crearsegmento_t segmento;
							segmento.idPCB = idPCB;
							segmento.tamanio = tamanio;
							crearSegmento(segmento);
							break;
						}
						case 4: {
							destruirsegmento_t segmento;
							segmento.idPCB = idPCB;
							destruirSegmentos(segmento);
							break;
						}
					}
				}
				log_destroy(opcional);
			}
			if (string_equals_ignore_case(token[0], "retardo")) {
				int retardo;
				if (token[1]) {
					retardo = atoi(token[1]);
					retardoGlobal = retardo * 1000;
					printf("Modificado el retardo\n");
					log_trace(logger, "Retardo: %d", retardo);
				}

			}
			if (string_equals_ignore_case(token[0], "algoritmo")) {
				if (string_equals_ignore_case(token[1], "FIRST_FIT")) {
					algoritmo = string_duplicate("FIRST_FIT");
					log_trace(logger, "Algoritmo cambiado a FIRST FIT");
				} else if (string_equals_ignore_case(token[1], "WORST_FIT")) {
					algoritmo = string_duplicate("WORST_FIT");
					log_trace(logger, "Algoritmo cambiado a WORST FIT");
				}

			}
			if (string_equals_ignore_case(token[0], "compactacion")) {
				compactacion();

			}
			if (string_equals_ignore_case(token[0], "dump")) {
				t_log *dump = log_create("Dump", "UMV", false, LOG_LEVEL_INFO);
				printf("1 Estructuras de Memoria \n2 Memoria Principal \n3 Contenido de la Memoria Principal \n");
				printf("Ingrese una opcion:");
				int opcion;
				scanf("%d", &opcion);
				switch (opcion) {
					case 1: {
						printf("1 Mostrar tablas de segmentos de un proceso \n2 Mostrar todas las tablas de segmentos\n");

						scanf("%d", &opcion);
						switch (opcion) {
							case 1: {
								printf("Ingrese el Id de Proceso:");
								scanf("%d", &opcion);
								printf("Guardar en Archivo?[s/n]\n");
								char *opFile = malloc(2);
								scanf("%s", opFile);

								int k;
								for (k = 0; k < auxTamanioBloqueMemoria; k++) {
									if ((&segmentosMemoria[k])->idPCB == opcion && (&segmentosMemoria[k])->virtualExterna != -9999) {
										int tamanio;
										if (buscarDireccion((&segmentosMemoria[k])->virtualExterna, &tamanio) != -1) {
											printf("Proceso: %d Direccion Virtual: %d Direccion Logica: %d Tamanio: %d \n", opcion, (&segmentosMemoria[k])->virtualExterna, k, tamanio);
											if (string_equals_ignore_case(opFile, "s")) {

												log_info(dump, "Proceso: %d Direccion Virtual: %d Direccion Logica: %d Tamanio: %d", opcion, (&segmentosMemoria[k])->virtualExterna, k, tamanio);
											}

										}
									}
								}
								if (string_equals_ignore_case(opFile, "s"))
									printf("Guardado\n");
								free(opFile);
								break;
							}
							case 2: {
								printf("Guardar en Archivo?[s/n]\n");
								char *opFile = malloc(2);
								scanf("%s", opFile);
								int z;
								for (z = 0; z < auxTamanioBloqueMemoria; z++) {

									if ((&segmentosMemoria[z])->virtualExterna != -9999 && (&segmentosMemoria[z])->virtualExterna != 0 && (&segmentosMemoria[z])->idPCB != -1) {
										int tamanio;
										if (buscarDireccion((&segmentosMemoria[z])->virtualExterna, &tamanio) != -1) {

											printf("Proceso: %d Direccion Virtual del segmento: %d Direccion Logica: %d Tamanio: %d\n", (&segmentosMemoria[z])->idPCB,
													(&segmentosMemoria[z])->virtualExterna, z, tamanio);
											if (string_equals_ignore_case(opFile, "s")) {
												log_info(dump, "Proceso: %d Direccion Virtual: %d Direccion Logica: %d Tamanio: %d", (&segmentosMemoria[z])->idPCB,
														(&segmentosMemoria[z])->virtualExterna, z, tamanio);
											}
										}
									}
								}
								if (string_equals_ignore_case(opFile, "s"))
									printf("Guardado\n");
								free(opFile);
								break;
							}
						}

						break;
					}
					case 2: {
						int cont = 0;
						int p;
						/*for (p = 0; p < auxTamanioBloqueMemoria; p++) {
						 if ((&segmentosMemoria[p])->virtualExterna != -9999 && (&segmentosMemoria[p])->virtualExterna != 0 &&(&segmentosMemoria[p])->idPCB!=-1) {
						 if (ant != (&segmentosMemoria[p])->idPCB) {
						 ant = (&segmentosMemoria[p])->idPCB;
						 segm = 1;
						 } else {
						 segm++;
						 }

						 printf("Programa: %d Segmento: %d Direccion Logica: %d \n", (&segmentosMemoria[p])->idPCB, segm, p);
						 cont = 1;
						 } else if ((&segmentosMemoria[p])->virtualExterna == -9999 || (&segmentosMemoria[p])->virtualExterna == 0 ||(&segmentosMemoria[p])->idPCB==-1) {
						 cont++;
						 }
						 }*/
						for (p = 0; p < auxTamanioBloqueMemoria; p++) {
							if ((&segmentosMemoria[p])->idPCB != -1) {
								cont++;
							}
						}
						printf("Espacio Libre: %d \n", auxTamanioBloqueMemoria - cont);

						break;
					}
					case 3: {

						printf("Indique un offset: ");
						scanf("%d", &opcion);
						int b = (int) mallocInit + opcion;
						void *variable;
						variable = (int *) b;
						printf("Indique un tamanio: ");
						int tamanio, r;

						scanf("%d", &tamanio);
						if (opcion + tamanio <= auxTamanioBloqueMemoria) {
							for (r = opcion; r < tamanio + opcion; r++) {
								printf("En la posicion de memoria %d hay: %s \n", r, string_substring((char *) variable, 0, 1));
								b++;
								variable = (int *) b;
							}
						}
						break;
					}
				}
			}
			if (string_equals_ignore_case(token[0], "help")) {
				printf("-operacion ID BASE OFFSET TAMANIO \n");
				printf("Ejemplo: operacion 1 83840 0 100 \n");
				printf("-retardo MILISEGUNDOS \n");
				printf("Ejemplo: retardo 2000\n");
				printf("-algoritmo FIRST_FIT | WORST_FIT\n");
				printf("-compactacion \n");
				printf("-dump\n");
			}
			if (string_equals_ignore_case(token[0], "clear"))
				system("clear");
			printf("UMV>");
		}
	}
	return NULL ;
}
void compactacion() {
	pthread_mutex_lock(&compactacionMutex);
	segmentos_memoria_t *memAux = (segmentos_memoria_t *) malloc(auxTamanioBloqueMemoria * sizeof(segmentos_memoria_t));

	int i;
	for (i = 0; i < auxTamanioBloqueMemoria; i++) {
		(&memAux[i])->idPCB = -1;

	}
	int j = auxTamanioBloqueMemoria - 1;
	int cont = 1;
	for (i = auxTamanioBloqueMemoria - 1; i >= 0; i--) {
		if ((&segmentosMemoria[i])->idPCB != -1) {
			(&memAux[j])->idPCB = (&segmentosMemoria[i])->idPCB;
			(&memAux[j])->virtualExterna = (&segmentosMemoria[i])->virtualExterna;
			(&memAux[j])->direccionEnMemoria = (int) mallocInit + j;

			if ((&memAux[j])->virtualExterna != -9999) {
				void *destino, *origen;

				origen = (int *) (&segmentosMemoria[i])->direccionEnMemoria;
				destino = (int *) (&memAux[j])->direccionEnMemoria;
				memcpy(destino, origen, cont);
				cont = 1;
			} else {
				cont++;
			}
			j--;
		}
	}
	segmentosMemoria = memAux;

	log_trace(logger, "Se realizo Compactacion");
	pthread_mutex_unlock(&compactacionMutex);
}

void iniciarUMV(char **cadena) {

	t_config *miConfig = config_create(cadena[1]);
	algoritmo = malloc(10);
	char *auxTam = malloc(20);
	IP = malloc(16);
	PUERTO = malloc(6);
	LOG_LEVEL = malloc(20);
	CONSOLA_ACTIVA = malloc(6);
	obtenerValoresConfiguracion(algoritmo, "ALGORITMO", 10, miConfig);
	obtenerValoresConfiguracion(auxTam, "TAMANIO_BLOQUE_DE_MEMORIA", 20, miConfig);
	auxTamanioBloqueMemoria = atoi(auxTam);

	obtenerValoresConfiguracion(IP, "IP_UMV", 16, miConfig);
	obtenerValoresConfiguracion(PUERTO, "PUERTO_UMV", 6, miConfig);
	obtenerValoresConfiguracion(CONSOLA_ACTIVA, "CONSOLA_ACTIVA", 6, miConfig);
	obtenerValoresConfiguracion(LOG_LEVEL, "LOG_LEVEL", 20, miConfig);
	config_destroy(miConfig);
	if (string_equals_ignore_case(CONSOLA_ACTIVA, "true"))
		logger = log_create(log_file_name, "UMV", true, log_level_from_string(LOG_LEVEL));
	else {
		logger = log_create(log_file_name, "UMV", false, log_level_from_string(LOG_LEVEL));
	}
	mallocInit = malloc(auxTamanioBloqueMemoria);
	int i;
	segmentosMemoria = (segmentos_memoria_t *) malloc(auxTamanioBloqueMemoria * sizeof(segmentos_memoria_t));

	for (i = 0; i < auxTamanioBloqueMemoria; i++) {
		(&segmentosMemoria[i])->idPCB = -1;
	}
	log_trace(logger, "Se inicio con %s y Tamanio de Memoria %s", algoritmo, auxTam);
	free(auxTam);
}

void *recibirPorSockets() {

	struct addrinfo *serverInfo;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hints.ai_flags = AI_PASSIVE;		// Asigna el address del localhost: 127.0.0.1
	hints.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP

	getaddrinfo(NULL, PUERTO, &hints, &serverInfo); // Notar que le pasamos NULL como IP, ya que le indicamos que use localhost en AI_PASSIVE

	int listenningSocket;
	listenningSocket = socket(serverInfo->ai_family, serverInfo->ai_socktype, serverInfo->ai_protocol);

	bind(listenningSocket, serverInfo->ai_addr, serverInfo->ai_addrlen);
	freeaddrinfo(serverInfo); // Ya no lo vamos a necesitar
	int status = 1;
	int errno = 0;
	while (status != 0 && errno == 0) {
		errno = listen(listenningSocket, BACKLOG); // IMPORTANTE: listen() es una syscall BLOQUEANTE.

		struct sockaddr_in addr; // Cliente. IP, puerto, etc.
		socklen_t addrlen = sizeof(addr);
		int socketCliente = accept(listenningSocket, (struct sockaddr *) &addr, &addrlen);

		cabecera_t cabecera;
		status = recv(socketCliente, &cabecera, sizeof(cabecera), 0);

		switch (cabecera.identificador) {
			case IdHandshake: {
				idproceso_t idProceso;
				status = recv(socketCliente, &idProceso, sizeof(idProceso), 0);
				if (string_equals_ignore_case(idProceso.tipo, "kernel")) {
					log_trace(logger, "Handshake con KERNEL");
					pthread_t idHilo;
					log_debug(logger, "Creo hilo de atencion al KERNEL");
					pthread_create(&idHilo, NULL, conexionCliente, (void *) socketCliente);

				} else {
					log_trace(logger, "Handshake con CPU");
					pthread_t idHilo2;
					log_debug(logger, "Creo hilo de atencion al CPU");
					pthread_create(&idHilo2, NULL, conexionCliente, (void *) socketCliente);

				}

			}

		}

		//close(socketCliente);
	}

	//close(socketCliente);
	return NULL ;
}
void *conexionCliente(void *param) {

	int socketCliente = (int) param;

	cabecera_t cabecera;
	int status = 1;
	while (status != 0) {
		status = recv(socketCliente, &cabecera, sizeof(cabecera), 0);
		if (status != 0) {

			switch (cabecera.identificador) {
				case IdCrearSegmento: {
					pthread_mutex_lock(&crearSegmentMutex);
					usleep(retardoGlobal);
					crearsegmento_t segmento;
					status = recv(socketCliente, &segmento, sizeof(segmento), 0);
					if (status != 0) {
						int ok = crearSegmento(segmento);

						send(socketCliente, &ok, sizeof(ok), 0);
					}
					pthread_mutex_unlock(&crearSegmentMutex);
					break;
				}
				case IdDestruirSegmento: {
					pthread_mutex_lock(&destruirSegmentoMutex);
					usleep(retardoGlobal);
					destruirsegmento_t destruir_Segmentos;

					status = recv(socketCliente, &destruir_Segmentos, sizeof(destruir_Segmentos), 0);
					if (status != 0) {
						destruirSegmentos(destruir_Segmentos);
					}
					pthread_mutex_unlock(&destruirSegmentoMutex);
					break;
				}
				case IdProgramaActivo: {
					usleep(retardoGlobal);
					activoprograma_t activoPrograma;
					status = recv(socketCliente, &activoPrograma, sizeof(activoPrograma), 0);
					if (status != 0) {
						log_trace(logger, "Proceso Activo %d", activoPrograma.idPCB);
						programaActivo = activoPrograma.idPCB;
					}
					break;
				}
				case IdEnviarBytes: {
					pthread_mutex_lock(&enviarBytesMutex);
					usleep(retardoGlobal);
					bytes_t bytes;
					status = recv(socketCliente, &bytes, sizeof(bytes), 0);
					if (status != 0) {
						void *buffer = malloc(bytes.tamanio);

						recv(socketCliente, (void *) buffer, bytes.tamanio, 0);

						int ok = almacenarBytes(bytes, buffer);

						send(socketCliente, &ok, sizeof(ok), 0);
						free(buffer);
					}
					pthread_mutex_unlock(&enviarBytesMutex);
					break;
				}
				case IdSolicitarBytes: {
					usleep(retardoGlobal);
					bytes_t bytes;
					status = recv(socketCliente, &bytes, sizeof(bytes), 0);
					if (status != 0) {
						void *buffer = malloc(bytes.tamanio);
						int ok = solicitarBytes(bytes, buffer);
						if (ok != -1) {
							send(socketCliente, &ok, sizeof(int), 0);
							send(socketCliente, buffer, bytes.tamanio, 0);
							((char *) buffer)[bytes.tamanio - 1] = 0;

							log_trace(logger, "Envio buffer al cpu %s", (char *) buffer);
						} else {
							send(socketCliente, &ok, sizeof(int), 0);
							log_error(logger, "Segmentation Fault");
						}

						free(buffer);
					}
					break;
				}
			}
		}

	}
	close(socketCliente);
	return NULL ;
}

int almacenarBytes(bytes_t bytes, void *buffer) {
	int tamanio = 0;
	int aux = buscarDireccion(bytes.base, &tamanio);
	if (aux != -1 && bytes.tamanio + bytes.offset <= tamanio) {
		void *variable;
		aux += bytes.offset;
		variable = (int *) aux;
		memcpy(variable, buffer, bytes.tamanio);
		return 1;

	} else {
		return -1;
		log_error(logger, "Memory Overload");
	}
	return 0;
}
int buscarDireccion(int base, int *tamanio) {
	*tamanio = 0;
	int i;
	for (i = auxTamanioBloqueMemoria - 1; i >= 0 && base != (&segmentosMemoria[i])->virtualExterna; i--) {

		if ((&segmentosMemoria[i])->virtualExterna == -9999) {
			*tamanio += 1;
		} else {
			*tamanio = 0;
		}

	}
	if ((&segmentosMemoria[i])->virtualExterna == base) {
		*tamanio += 1;
		return (&segmentosMemoria[i])->direccionEnMemoria;
	} else {
		return -1;
	}

}
int solicitarBytes(bytes_t bytes, void *buffer) {

	int tamanio;

	int aux = buscarDireccion(bytes.base, &tamanio);
	if (aux != -1 && bytes.tamanio + bytes.offset <= tamanio) {

		void *variable;
		aux += bytes.offset;					// = malloc(bytes.tamanio);
		variable = (int *) aux;
		memcpy(buffer, variable, bytes.tamanio);
		return 1;
	} else {
		return -1;
	}

}

void destruirSegmentos(destruirsegmento_t destruir_segmentos) {
	bool algoDestruido = false;
	int i;
	for (i = 0; i < auxTamanioBloqueMemoria; i++) {
		if ((&segmentosMemoria[i])->idPCB == destruir_segmentos.idPCB) {
			(&segmentosMemoria[i])->idPCB = -1;
			(&segmentosMemoria[i])->virtualExterna = 0;
			algoDestruido = true;
		}
	}
	if (algoDestruido)
		log_trace(logger, "Segmento Destruido PID: %d", destruir_segmentos.idPCB);

}
int crearSegmento(crearsegmento_t segmento) {

	programaActivo = segmento.idPCB;
	if (string_equals_ignore_case(algoritmo, "FIRST_FIT")) {

		int mayorEspacio = 0;
		int posicion = 0;
		calcularTamanioDisponible(&posicion, &mayorEspacio, segmento.tamanio);

		log_trace(logger, "PID:%d ,Mayor Espacio: %d, Tamanio segmento a crear: %d, Posicion: %d ", programaActivo, mayorEspacio, segmento.tamanio, posicion);

		if (mayorEspacio >= segmento.tamanio) {

			return alojarSegmentos(posicion, segmento.tamanio);
		} else {
			compactacion();
			mayorEspacio = 0;
			posicion = 0;
			calcularTamanioDisponible(&posicion, &mayorEspacio, segmento.tamanio);
			log_trace(logger, "PID:%d ,Mayor Espacio: %d, Tamanio segmento a crear: %d, Posicion: %d ", programaActivo, mayorEspacio, segmento.tamanio, posicion);

			if (mayorEspacio >= segmento.tamanio) {

				return alojarSegmentos(posicion, segmento.tamanio);
			}
		}

	} else {

		int mayorEspacio = 0;
		int posicion = 0;
		calcularTamanio_WORST_FIT(&posicion, &mayorEspacio);
		if (mayorEspacio >= segmento.tamanio) {
			return alojarSegmentos(posicion, segmento.tamanio);
		} else {
			mayorEspacio = 0;
			posicion = 0;
			compactacion();
			calcularTamanio_WORST_FIT(&posicion, &mayorEspacio);
			if (mayorEspacio >= segmento.tamanio) {
				return alojarSegmentos(posicion, segmento.tamanio);
			}
		}

	}
	return -1;
}
void calcularTamanioDisponible(int *posicion, int *mayorEspacio, int tamanio) {
	int i;
	int cont = 0;
	for (i = 0; (tamanio <= *mayorEspacio) ^ (i < auxTamanioBloqueMemoria); i++) {
		if ((&segmentosMemoria[i])->idPCB == -1) {
			cont++;

		} else if (cont > *mayorEspacio) {
			*mayorEspacio = cont;
			*posicion = i - cont;
			cont = 0;
		} else {
			cont = 0;
		}

	}
	if (cont > *mayorEspacio) {
		*mayorEspacio = cont;
		*posicion = i - cont;
		cont = 0;
	}
}

void calcularTamanio_WORST_FIT(int *posicion, int *mayorEspacio) {
	int i;
	int cont = 0;
	for (i = 0; (i < auxTamanioBloqueMemoria); i++) {
		if ((&segmentosMemoria[i])->idPCB == -1) {
			cont++;

		} else if (cont > *mayorEspacio) {
			*mayorEspacio = cont;
			*posicion = i - cont;
			cont = 0;
		} else {
			cont = 0;
		}

	}
	if (cont > *mayorEspacio) {
		*mayorEspacio = cont;
		*posicion = i - cont;
		cont = 0;
	}
}
int alojarSegmentos(int posicion, int tamanio) {

	int aux;
	int contador = 0;
	int j;
	for (j = posicion; j < (posicion + tamanio); j++) {

		(&segmentosMemoria[j])->idPCB = programaActivo;

		if (contador == 0) {
			aux = (rand() % 100001);
			int p;
			for (p = 0; p < auxTamanioBloqueMemoria; p++) {
				if ((&segmentosMemoria[j])->virtualExterna == aux)
					aux = (rand() % 10000);
			}
			(&segmentosMemoria[j])->virtualExterna = aux;
			(&segmentosMemoria[j])->direccionEnMemoria = (int) mallocInit + j;
			//printf("Direccion:%d \n", (int) (mallocInit + j));
		} else {
			(&segmentosMemoria[j])->virtualExterna = -9999;
			(&segmentosMemoria[j])->direccionEnMemoria = (int) (mallocInit + j);
			//printf("Direccion:%d \n", (int) (mallocInit + j));

		}
		contador++;

	}
	return aux;

}

