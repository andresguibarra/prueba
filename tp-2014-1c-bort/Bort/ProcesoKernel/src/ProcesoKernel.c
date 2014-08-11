/*
 * kernelcode.c
 *
 *  Created on: 05/05/2014
 *      Author: utnso
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "Serial.h"
#include <commons/config.h>
#include <commons/string.h>
#include <commons/error.h>
#include <commons/collections/list.h>
#include <parser/parser.h>
#include <parser/metadata_program.h>
#include <commons/log.h>

#define MAXMSG 10000
#define BACKLOG 5			// Define cuantas conexiones vamos a mantener pendientes al mismo tiempo
#define PACKAGESIZE 10000	// Define cual va a ser el size maximo del paquete a enviar
char **dispositivos_ES;
char **tiempos_ES;
char **semaforos;
int *valores_semaforos;
char **variables_globales;
int *valores_globales;
char *puertoProg;
char *puertoCpu;
char *puertoUmv;
char *ipUmv;
int quantum;
int retardo;
int multiprogramacion;
int tamanioStack;

typedef struct ProcessControlBlock {
	uint32_t pid;
	uint32_t codigo_segmento;
	uint32_t stack_segmento;
	uint32_t dir_contexto;

	uint32_t indice_etiquetas;
	uint32_t programCounter;
	uint32_t tamanio_contexto;
	uint32_t tamanio_indice_etiquetas;
	uint32_t indice_codigo;
	int sock;
} pcb_t;

typedef struct parametro_hiloES {
	char *dispositivo;
	int retardoDisp;
	int cola;
} param_hiloES;
typedef struct proceso_colaNew {
	int pid;
	t_metadata_program* metadata;
	char* codigo;
	int peso;
	int socketPrograma;

} colaNew_t;
typedef struct proceso_colaExec {
	pcb_t pcb;
	int cpu; //socket del cpu

} colaExec_t;
typedef struct proceso_colaBlock {
	pcb_t pcb;
	char* bloqueado; //Indica con que esta bloqueado (Disco, impresora, semaforo1, semaforo2, etc).

} colaBlock_t;

typedef struct proceso_es {
	pcb_t pcb;
	int instantes;

} procesoES_t;

void configurarKernel(char **config);
char* obtenerValoresConfiguracion(char *expected, t_config *config);
void *plp(void *param);
void *pcp(void *param);
void *HIO(void *info);
int crearSegmento(int socketUmv, int pid, int tamanio);
int enviarDatos(int sock, int base, int offset, int tamanio, void *buffer);
pcb_t *crearPCB(int socketPrograma, int socketUmv, t_metadata_program *metadataCodigo, char *codigoLiteral, int pid);
void rechazarPrograma();
void borrarPCB(int socketUmv, int pid);
void pasarReady();
void enviaraCPU(int socket_cpu);
void revisarCPUvacios();
void imprimirColas();
int multiprogramacionDisponible();
int detectarColaDispositivo(char* dispositivo);
int tamanioVectorChar(char **vec);
int obtenerPosicionEnArrayString(char* elemento, char** array);
int obtenerSockProgPorCpu(int cpu);
int detectarIndiceProcesoBloqueado(pcb_t* pcb);
pcb_t* cortarEjecucion(int cpu);
int quitarDeColas(int socketProg, int socketUmv);
int sacarDeNew(int socketProg);
int sacarDeReady(int socketProg);
int sacarDeBlock(int socketProg);
int sacarDeExec(int socketProg);
bool estaEnExit(int pid);
void sacarDeES(int pid);
void sacarDeSEM(int pid);
void sacarCPUDeVacios(int cpu);
void imprimirTextoEnProg(char* texto, int socketPrograma, int tamanio);
void enviarOrdenFinalizarProg(int socketPrograma);
void agregarCpuVacio(int sockCpu);
void agregarAExit(int pid);
int tamanioReady();

pthread_mutex_t mutex_new = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_ready = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_block = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_exit = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_exec = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_cpuvacio = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_es = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_sem = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_progmemoria = PTHREAD_MUTEX_INITIALIZER;

unsigned int pidAux = 0;
int programas_en_memoria = 0;
t_list *cola_new;
t_list *cola_ready;
t_list *cola_block;
t_list *cola_exit;
t_list *cola_exec;
t_list *cola_sem; //lista de colas de semaforos
t_list *CPU_vacio;
t_list *cola_es; //Lista de colas de I/O

t_log* logger;
char* archivo_log = "log_Kernel";
int socketUmv;

int main(int argc, char **argv) {
	logger = log_create(archivo_log, "Kernel", false, LOG_LEVEL_TRACE);
	log_info(logger, "Kernel iniciado.");
	configurarKernel(argv);
	log_info(logger, "Se obtuvieron los valores de configuracion.");
	cola_new = list_create(); //Crea las colas
	cola_ready = list_create();
	cola_exit = list_create();
	cola_block = list_create();
	cola_exec = list_create();

	cola_sem = list_create();
	cola_es = list_create();

	int i = 0;

	param_hiloES *info_hiloES = malloc(sizeof(param_hiloES) * tamanioVectorChar(dispositivos_ES));

	while (dispositivos_ES[i] != NULL ) {
		t_list *aux;
		aux = list_create();
		list_add(cola_es, aux);

		info_hiloES[i].cola = i;
		info_hiloES[i].dispositivo = dispositivos_ES[i];

		info_hiloES[i].retardoDisp = atoi(tiempos_ES[i]);
		pthread_t idHiloES;

		pthread_create(&idHiloES, NULL, HIO, (void *) &info_hiloES[i]);

		i++;

	}
	log_info(logger, "Se ha creado un hilo por cada dispositivo de I/O");
	i = 0;
	while (semaforos[i] != NULL ) {
		t_list *aux;
		aux = list_create();
		list_add(cola_sem, aux);
		i++;
	}
	log_info(logger, "Se ha creado creado una lista por cada semaforo, y se han agregado a la cola de semaforos.");

	struct addrinfo hintsProg;
	struct addrinfo *serverInfoProg;

	struct addrinfo hintsUmv;
	struct addrinfo *serverInfoUmv;

	struct addrinfo hintsCpu;
	struct addrinfo *serverInfoCpu;

	memset(&hintsProg, 0, sizeof(hintsProg));
	hintsProg.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hintsProg.ai_flags = AI_PASSIVE;		// Asigna el address del localhost: 127.0.0.1
	hintsProg.ai_socktype = SOCK_STREAM;		// Indica que usaremos el protocolo TCP
	getaddrinfo(NULL, puertoProg, &hintsProg, &serverInfoProg);

	memset(&hintsUmv, 0, sizeof(hintsUmv));
	hintsUmv.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hintsUmv.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP
	getaddrinfo(ipUmv, puertoUmv, &hintsUmv, &serverInfoUmv);

	memset(&hintsCpu, 0, sizeof(hintsCpu));
	hintsCpu.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hintsCpu.ai_flags = AI_PASSIVE;	// Asigna el address del localhost: 127.0.0.1
	hintsCpu.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP
	getaddrinfo(NULL, puertoCpu, &hintsCpu, &serverInfoCpu);

	/*	Mediante socket(), obtengo el File Descriptor que me proporciona el sistema (un integer identificador)*/
	/* Obtenemos el socket que escuche las conexiones entrantes */

	int listenningSocketProg = socket(serverInfoProg->ai_family, serverInfoProg->ai_socktype, serverInfoProg->ai_protocol);
	socketUmv = socket(serverInfoUmv->ai_family, serverInfoUmv->ai_socktype, serverInfoUmv->ai_protocol);
	int listenningSocketCpu = socket(serverInfoCpu->ai_family, serverInfoCpu->ai_socktype, serverInfoCpu->ai_protocol);

	/*le digo por que "telefono" tiene que esperar las llamadas.*/
	bind(listenningSocketProg, serverInfoProg->ai_addr, serverInfoProg->ai_addrlen);
	bind(listenningSocketCpu, serverInfoCpu->ai_addr, serverInfoCpu->ai_addrlen);
	connect(socketUmv, serverInfoUmv->ai_addr, serverInfoUmv->ai_addrlen);

	freeaddrinfo(serverInfoProg);
	freeaddrinfo(serverInfoUmv);
	freeaddrinfo(serverInfoCpu); // Ya no los vamos a necesitar

	cabecera_t cabecera;
	idproceso_t idProceso;

	strcpy(idProceso.tipo, "kernel");
	cabecera.identificador = IdHandshake;
	send(socketUmv, &cabecera, sizeof(cabecera), 0);
	send(socketUmv, &idProceso, sizeof(idproceso_t), 0);

	imprimirColas();

	pthread_t idHiloCpu;
	pthread_t idHiloProg;

	pthread_create(&idHiloCpu, NULL, pcp, (void *) &listenningSocketCpu);
	pthread_create(&idHiloProg, NULL, plp, (void *) &listenningSocketProg);

	pthread_join(idHiloCpu, NULL );
	pthread_join(idHiloProg, NULL );

	free(info_hiloES);
	return 0;

}
char* obtenerValoresConfiguracion(char *expected, t_config *config) {
	if (config_has_property(config, expected)) {
		char * aux = config_get_string_value(config, expected);
		string_to_lower(aux);
		return aux;

	}
	else {
		error_show("No se encuentra %s en el archivo de configuracion. \n", expected);
		return NULL ;
	}
}
void configurarKernel(char **config) {

	t_config *kernelConfig = config_create(config[1]);

	puertoProg = obtenerValoresConfiguracion("PUERTO_PROG", kernelConfig);
	puertoCpu = obtenerValoresConfiguracion("PUERTO_CPU", kernelConfig);
	puertoUmv = obtenerValoresConfiguracion("PUERTO_UMV", kernelConfig);
	ipUmv = obtenerValoresConfiguracion("IP_UMV", kernelConfig);
	quantum = atoi(obtenerValoresConfiguracion("QUANTUM", kernelConfig));
	retardo = atoi(obtenerValoresConfiguracion("RETARDO", kernelConfig));
	multiprogramacion = atoi(obtenerValoresConfiguracion("MULTIPROGRAMACION", kernelConfig));
	tamanioStack = atoi(obtenerValoresConfiguracion("TAMANIO_STACK", kernelConfig));

	dispositivos_ES = string_get_string_as_array(obtenerValoresConfiguracion("ID_HIO", kernelConfig));
	tiempos_ES = string_get_string_as_array(obtenerValoresConfiguracion("TIEMPOS_HIO", kernelConfig));
	semaforos = string_get_string_as_array(obtenerValoresConfiguracion("SEMAFOROS", kernelConfig));
	valores_semaforos = malloc(sizeof(int) * tamanioVectorChar(semaforos));
	char** aux_valores_semaforos = string_get_string_as_array(obtenerValoresConfiguracion("VALOR_SEMAFORO", kernelConfig));
	int i;
	for (i = 0; i < tamanioVectorChar(semaforos); i++) {
		valores_semaforos[i] = atoi(aux_valores_semaforos[i]);
	}

	variables_globales = string_get_string_as_array(obtenerValoresConfiguracion("VARIABLES_GLOBALES", kernelConfig));
	valores_globales = malloc(sizeof(int) * tamanioVectorChar(variables_globales));

	for (i = 0; i < tamanioVectorChar(variables_globales); i++) {
		valores_globales[i] = 0;
	}
}

void *plp(void *param) {
	log_info(logger, "PLP iniciado");
	int *listenningSocketProg = (int *) param;

	fd_set active_fd_set, read_fd_set;
	int i;
	struct sockaddr_in clientname;
	socklen_t size;

	int errno = listen(*listenningSocketProg, BACKLOG);

	FD_ZERO(&active_fd_set);
	FD_SET(*listenningSocketProg, &active_fd_set);

	int escuchar = 1;
	while (escuchar && !errno) {

		FD_ZERO(&read_fd_set);
		read_fd_set = active_fd_set;

		if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL ) < 0) {
			perror("select");
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < FD_SETSIZE; ++i) { // FD_SETSIZE vale 1024 por defecto

			if (FD_ISSET (i, &read_fd_set)) { //FD_ISSET Retorna True si i (socket) esta en el set.

				if (i == *listenningSocketProg) { //Compara si i es el socket por el que estaba escuchando
					//Y si es, escuchamos la nueva conexion y lo agregamos a la lista
					log_info(logger, "Nuevo programa conectado");
					int new;
					size = sizeof(clientname);

					new = accept(i, (struct sockaddr *) &clientname, &size);
					if (new < 0) {
						perror("accept");
						exit(EXIT_FAILURE);
					}

					FD_SET(new, &active_fd_set);
				}
				else {
					log_info(logger, "Mensaje de programa");
					cabecera_t cabecera; //= malloc(sizeof(cabecera_t));

					int status = recv(i, &cabecera, sizeof(cabecera_t), 0);

					switch (cabecera.identificador) {

					case Kernel_Request: {
						log_info(logger, "Kernel Request");
						cabecera.identificador = Programa_OK;
						send(i, &cabecera, sizeof(cabecera_t), 0);
						break;
					}

					}
					//free(cabecera);

					if (status > 0) {
						int tamanio;

						status = recv(i, &tamanio, 4, 0); //Recibe el tamaño
						if (status > 0) {
							char *package = malloc(PACKAGESIZE);
							status = recv(i, (void*) package, tamanio, 0); //Recibe el codigo literal
							if (status > 0) {
								log_info(logger, "Codigo recibido");
								t_metadata_program* metadataCodigo = malloc(sizeof(t_metadata_program));

								colaNew_t* procesoEntrante = malloc(sizeof(colaNew_t));
								colaNew_t *aux = malloc(sizeof(colaNew_t));

								metadataCodigo = metadata_desde_literal(package);

								pidAux++;
								procesoEntrante->pid = pidAux;
								procesoEntrante->peso = 5 * metadataCodigo->cantidad_de_etiquetas + 3 * metadataCodigo->cantidad_de_funciones + metadataCodigo->instrucciones_size;
								procesoEntrante->codigo = package;
								procesoEntrante->metadata = metadataCodigo;
								procesoEntrante->socketPrograma = i;

								pthread_mutex_lock(&mutex_new);
								bool encolado = 0;
								int k;
								for (k = 0; k < list_size(cola_new); k++) {
									aux = list_get(cola_new, k);

									if (aux->peso < procesoEntrante->peso && encolado == 0) {

										list_add_in_index(cola_new, k, procesoEntrante);
										aux = list_get(cola_new, k);
										encolado = 1;
									}

								}
								if (encolado == 0) {
									list_add(cola_new, procesoEntrante);

								}
								log_info(logger, "Se ha encolado el proceso en New");
								pthread_mutex_unlock(&mutex_new);
								imprimirColas();

							}
						}
					}

					else { //Si el programa cerro la conexion, saca el socket de la lista y el proceso de la cola de new y de la umv.
						log_info(logger, "Un programa ha cerrado la conexion");
						FD_CLR(i, &active_fd_set);

						int pid = quitarDeColas(i, socketUmv);

						if (pid > 0) {

							agregarAExit(pid);
							borrarPCB(socketUmv, pid);

						}

						imprimirColas();

					}
				}
			}

		}

		log_info(logger, "plp pasarReady");
		pasarReady();

		log_info(logger, "plp Revisar cpu vacios");
		revisarCPUvacios();
		imprimirColas();

	}
	close(socketUmv);
	close(*listenningSocketProg);

	//free(aux);

	return 0;
}

void *pcp(void *param) {
	log_info(logger, "PCP iniciado");
	int* listenningSocketCpu = (int *) param;
	CPU_vacio = list_create(); //Lista que tiene los socket de los CPU que estan al pedo.
	fd_set active_fd_set, read_fd_set;
	int i;
	struct sockaddr_in clientname;
	socklen_t size;

	int errno = listen(*listenningSocketCpu, BACKLOG);

	FD_ZERO(&active_fd_set);
	FD_SET(*listenningSocketCpu, &active_fd_set);

	int escuchar = 1;
	while (escuchar && !errno) {

		FD_ZERO(&read_fd_set);
		read_fd_set = active_fd_set;

		if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL ) < 0) {

			perror("select");
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < FD_SETSIZE; ++i) // FD_SETSIZE vale 1024 por defecto
			if (FD_ISSET (i, &read_fd_set)) { //FD_ISSET Retorna True si i (socket) esta en el set.

				if (i == *listenningSocketCpu) { //Compara si i es el socket por el que estaba escuchando
					//Y si es, escuchamos la nueva conexion y lo agregamos a la lista

					int new;
					size = sizeof(clientname);
					new = accept(i, (struct sockaddr *) &clientname, &size);
					if (new < 0) {
						perror("accept");
						exit(EXIT_FAILURE);
					}

					log_info(logger, "Nuevo CPU conectado");
					FD_SET(new, &active_fd_set);
					int* auxNew = malloc(sizeof(int));
					*auxNew = new;
					pthread_mutex_lock(&mutex_cpuvacio);
					list_add(CPU_vacio, auxNew);
					pthread_mutex_unlock(&mutex_cpuvacio);
					send(new, &quantum, sizeof(quantum), 0);
					send(new, &retardo, sizeof(retardo), 0);
				}
				else {
					//SINO ES EL SOCKET POR EL QUE ESTAMOS ESCUCHANDO, RECIBE.
					log_info(logger, "Mensaje de CPU");
					cabecera_t cabecera;

					int status = recv(i, &cabecera, sizeof(cabecera_t), 0); //Recibe la cabecera
					if (status > 0) {
						switch (cabecera.identificador) {
						case Quantum_cumplido:
							log_info(logger, "Quantum cumplido");

							if (tamanioReady() > 0) {

								pcb_t *pcb_saliente = malloc(sizeof(pcb_t));

								sacarDeExec(obtenerSockProgPorCpu(i));

								enviaraCPU(i); //Envia el nuevo PCB

								status = recv(i, pcb_saliente, sizeof(pcb_t), 0);
								if (status > 0 && estaEnExit(pcb_saliente->pid) == false) {
									pthread_mutex_lock(&mutex_ready);
									list_add(cola_ready, pcb_saliente);
									pthread_mutex_unlock(&mutex_ready);
									log_info(logger, "Kernel REEMPLAZA programa con otro");

								}
								imprimirColas();

							}
							else {

								cabecera.identificador = CPU_Continuar;
								send(i, &cabecera, sizeof(cabecera_t), 0);
								log_info(logger, "Kernel envia CONTINUAR con mismo programa");
							}
							break;

						case Id_ProgramaFinalizado:

							log_info(logger, "Finalizo un programa");
							pcb_t *pcb_saliente = malloc(sizeof(pcb_t));
							status = recv(i, pcb_saliente, sizeof(pcb_t), 0);
							if (status > 0 && estaEnExit(pcb_saliente->pid) == false) {

								enviarOrdenFinalizarProg(pcb_saliente->sock);
								//	sacarDeExec(pcb_saliente->sock);

								//agregarAExit(pcb_saliente);

								if (tamanioReady() > 0) {

									enviaraCPU(i); //Envia el nuevo PCB e imprime colas

								}
								else {
									agregarCpuVacio(i);

								}
							}
							else {
								agregarCpuVacio(i);
							}

							break;

						case Kernel_ObtenerValorCompartida: {
							log_info(logger, "Peticion de obtener valor variable compartida");
							int tamanio, pos;
							status = recv(i, &tamanio, sizeof(int), 0);
							if (status > 0) {
								char*variable = malloc(tamanio);
								status = recv(i, variable, tamanio, 0);
								if (status > 0) {
									pos = obtenerPosicionEnArrayString(variable, variables_globales);
									send(i, &valores_globales[pos], sizeof(int), 0);
									free(variable);
								}
							}
							break;
						}
						case Kernel_AsignarValorCompartida: {
							log_info(logger, "Peticion de asignar valor variable compartida");
							int tamanio, pos, valor;
							status = recv(i, &tamanio, sizeof(int), 0);
							if (status > 0) {
								char* variable = malloc(tamanio);
								status = recv(i, variable, tamanio, 0);
								if (status > 0) {
									status = recv(i, &valor, sizeof(int), 0);
									if (status > 0) {
										pos = obtenerPosicionEnArrayString(variable, variables_globales);
										valores_globales[pos] = valor;
										free(variable);
									}
								}
							}
							break;
						}
						case Kernel_Imprimir: {
							log_info(logger, "Peticion para imprimir valor en programa");
							t_valor_variable *valor_variable = malloc(sizeof(t_valor_variable));
							int pid;
							status = recv(i, &pid, sizeof(int), 0);
							if (status > 0) {
								status = recv(i, valor_variable, sizeof(t_valor_variable), 0);
								if (status > 0) {
									int *sockPrograma = malloc(sizeof(int));

									status = recv(i, sockPrograma, sizeof(int), 0);
									if (status > 0 && estaEnExit(pid) == false) {

										cabecera_t* cabecera = malloc(sizeof(cabecera_t));
										cabecera->identificador = Programa_Imprimir;
										send(*sockPrograma, cabecera, sizeof(cabecera_t), 0);
										send(*sockPrograma, valor_variable, sizeof(int), 0);
										log_info(logger, "Imprimio valor en Programa");

										free(cabecera);
									}
									else {
										log_warning(logger, "No se pudo imprimir en prog");
										free(sockPrograma);
									}
								}
							}
							break;
						}
						case Kernel_ImprimirTexto: {
							log_info(logger, "Peticion para imprimir texto en programa");
							int *tamanio = malloc(sizeof(int));
							int *sockPrograma = malloc(sizeof(int));
							int pid;
							status = recv(i, &pid, sizeof(int), 0);
							if (status > 0) {
								status = recv(i, tamanio, sizeof(int), 0);
								if (status > 0) {
									void *buffer = malloc(*tamanio);
									status = recv(i, sockPrograma, sizeof(int), 0);
									if (status > 0) {
										status = recv(i, buffer, *tamanio, 0);
										if (status > 0 && estaEnExit(pid) == false) {
											((char *) buffer)[*tamanio - 1] = 0;

											imprimirTextoEnProg(buffer, *sockPrograma, *tamanio);
										}
									}
								}
							}
							free(sockPrograma);
							free(tamanio);

							break;
						}
						case Kernel_EntradaSalida: {
							log_info(logger, "Programa a entrada salida");
							pcb_t *pcb_saliente = malloc(sizeof(pcb_t));
							procesoES_t* procesoES = malloc(sizeof(procesoES_t));
							int tamanioTipoES;
							int instantes;
							status = recv(i, pcb_saliente, sizeof(pcb_t), 0);
							if (status > 0) {
								status = recv(i, &tamanioTipoES, sizeof(int), 0);
								if (status > 0) {

									char *tipoES = malloc(tamanioTipoES);
									status = recv(i, tipoES, tamanioTipoES, 0);
									if (status > 0) {
										status = recv(i, &instantes, sizeof(int), 0);
										if (status > 0 && estaEnExit(pcb_saliente->pid) == false) {
											procesoES->pcb = *pcb_saliente;
											procesoES->instantes = instantes;

											int disp = detectarColaDispositivo(tipoES);

											sacarDeExec(obtenerSockProgPorCpu(i));

											colaBlock_t* procesoBlock = malloc(sizeof(colaBlock_t));

											procesoBlock->pcb = *pcb_saliente;

											procesoBlock->bloqueado = tipoES;

											list_add(cola_block, procesoBlock);

											list_add(list_get(cola_es, disp), procesoES);

											if (tamanioReady() > 0) {

												enviaraCPU(i); //Envia el nuevo PCB e imprime colas

											}
											else {
												agregarCpuVacio(i);

											}
											imprimirColas();
											free(pcb_saliente);
										}
									}
								}
							}
							break;
						}
						case Kernel_Signal: {
							log_info(logger, "Signal semaforo");
							int tamanio, pos;
							status = recv(i, &tamanio, sizeof(int), 0);
							if (status > 0) {
								char* sem = malloc(tamanio);
								status = recv(i, sem, tamanio, 0);
								if (status > 0) {

									pos = obtenerPosicionEnArrayString(sem, semaforos);
									free(sem);
									valores_semaforos[pos]++;

									if (valores_semaforos[pos] <= 0 && list_size(list_get(cola_sem, pos)) > 0) {
										log_info(logger, "Se desbloquea un proceso por semaforo");
										list_add(cola_ready, list_get(list_get(cola_sem, pos), 0));
										log_info(logger, "a ready");
										list_remove(cola_block, detectarIndiceProcesoBloqueado(list_get(list_get(cola_sem, pos), 0)));
										log_info(logger, "saca de block");
										list_remove(list_get(cola_sem, pos), 0);
										imprimirColas();
									}
								}
							}
							break;
						}
						case Kernel_Wait: {
							log_info(logger, "Wait semaforo");
							int tamanio, pos, pid;
							status = recv(i, &pid, sizeof(int), 0);
							if (status > 0) {
								status = recv(i, &tamanio, sizeof(int), 0);
								if (status > 0) {
									char* sem = malloc(tamanio);
									status = recv(i, sem, tamanio, 0);
									if (status > 0) {

										pos = obtenerPosicionEnArrayString(sem, semaforos);

										free(sem);

										valores_semaforos[pos]--;

										if (valores_semaforos[pos] < 0 && estaEnExit(pid) == false) { //Si se bloquea
											log_info(logger, "Se bloquea el proceso por semaforo");
											sacarDeExec(obtenerSockProgPorCpu(i));
											colaBlock_t* procBloqueado = malloc(sizeof(colaBlock_t));
											pcb_t* aux = cortarEjecucion(i);
											if (aux != NULL ) {
												procBloqueado->pcb = *aux;
												free(aux);
												procBloqueado->bloqueado = semaforos[pos];
												agregarCpuVacio(i);
												list_add(cola_block, procBloqueado);
												list_add(list_get(cola_sem, pos), &procBloqueado->pcb);
												imprimirColas();
											}
											else {
												log_error(logger, "Se cerro CPU en medio de un corteEjecucion y no llego a devolver el pcb");
											}

										}
										else {
											cabecera_t* cabecera = malloc(sizeof(cabecera_t));
											cabecera->identificador = CPU_Continuar;
											send(i, cabecera, sizeof(cabecera_t), 0);

										}
									}
								}
							}
							break;
						}
						case CPU_Apagado: {
							log_info(logger, "CPU cierre programado");
							pcb_t *pcb_saliente = malloc(sizeof(pcb_t));
							status = recv(i, pcb_saliente, sizeof(pcb_t), 0);
							pthread_mutex_lock(&mutex_ready);
							list_add(cola_ready, pcb_saliente);
							pthread_mutex_unlock(&mutex_ready);
							sacarDeExec(pcb_saliente->sock);
							imprimirColas();
							break;
						}

						}

					}
					else if (status == 0) {
						//Si el cpu cerro la conexion, saca el socket de la lista.
						log_info(logger, "Un CPU ha cerrado la conexion");
						FD_CLR(i, &active_fd_set);
						sacarCPUDeVacios(i);
						int socketProg = obtenerSockProgPorCpu(i);
						if (socketProg > 0) { //Si habia un proceso ejecutandose en el cpu
							log_warning(logger, "El CPU tenia un programa ejecutandose");
							imprimirTextoEnProg("****Cpu desconectado durante ejecucion****", socketProg, sizeof("****Cpu desconectado durante ejecucion****"));
							enviarOrdenFinalizarProg(socketProg);
						}
					}

				}
			}

		log_info(logger, "pcp pasarReady");
		pasarReady();

		log_info(logger, "pcp revisar cpu vacios");
		revisarCPUvacios();
		imprimirColas();

	}
	close(*listenningSocketCpu);
	return 0;
}

void *HIO(void *param) {
	log_info(logger, "Se inicia hilo de I/O");
	param_hiloES *info = (param_hiloES *) param;

	while (true) {
		while (list_size(list_get(cola_es, info->cola)) > 0) {

			procesoES_t* aux = list_get(list_get(cola_es, info->cola), 0);

			usleep(info->retardoDisp * 1000 * aux->instantes); // *1000 porque usleep trabaja en microsegundos. y el retardo esta en milisegundos.

			log_info(logger, "Ha terminado una espera de I/O");
			pthread_mutex_lock(&mutex_block);
			pthread_mutex_lock(&mutex_ready);
			pthread_mutex_lock(&mutex_es);
			if (estaEnExit(aux->pcb.pid) == false) {
				list_add(cola_ready, &(aux->pcb)); //Manda el pcb a ready devuelta
			}
			list_remove(list_get(cola_es, info->cola), 0);

			list_remove(cola_block, detectarIndiceProcesoBloqueado(&(aux->pcb)));

			pthread_mutex_unlock(&mutex_block);
			pthread_mutex_unlock(&mutex_ready);
			pthread_mutex_unlock(&mutex_es);

			imprimirColas();
			revisarCPUvacios();
			imprimirColas();

		}

	}

	return 0;
}

void borrarPCB(int socketUmv, int pid) {
	log_info(logger, "Se Borra un PCB");
	cabecera_t cabecera;
	destruirsegmento_t *segmento = malloc(sizeof(destruirsegmento_t));
	cabecera.identificador = IdDestruirSegmento;
	send(socketUmv, &cabecera, sizeof(cabecera), 0);
	segmento->idPCB = pid;
	send(socketUmv, segmento, sizeof(destruirsegmento_t), 0); //Envia datos del pcb a eliminar.
	pthread_mutex_lock(&mutex_progmemoria);
	programas_en_memoria--;
	pthread_mutex_unlock(&mutex_progmemoria);
}

int crearSegmento(int socketUmv, int pid, int tamanio) {

	int *respuesta = malloc(sizeof(int));
	cabecera_t cabecera;
	crearsegmento_t *segmento = malloc(sizeof(crearsegmento_t));
	cabecera.identificador = IdCrearSegmento;

	send(socketUmv, &cabecera, sizeof(cabecera), 0);
	segmento->idPCB = pid;
	segmento->tamanio = tamanio;
	send(socketUmv, segmento, sizeof(crearsegmento_t), 0); //Envia datos del segmento a reservar.
	int status = recv(socketUmv, (void*) respuesta, sizeof(int), 0);
	if (status > 0)
		return *respuesta;
	else
		return -1;
}

int enviarDatos(int sock, int base, int offset, int tamanio, void *buffer) {

	bytes_t bytes;
	bytes.base = base;
	bytes.offset = offset;
	bytes.tamanio = tamanio;
	cabecera_t cabecera;
	cabecera.identificador = IdEnviarBytes;

	send(sock, &cabecera, sizeof(cabecera), 0);
	send(sock, &bytes, sizeof(bytes), 0);
	int ok;
	send(sock, (void *) buffer, bytes.tamanio, 0);

	int status = recv(sock, &ok, sizeof(ok), 0);

	if (status > 0)
		return ok;
	else
		return -1;
}

pcb_t *crearPCB(int socketPrograma, int socketUmv, t_metadata_program *metadataCodigo, char *codigoLiteral, int pid) {
	pthread_mutex_lock(&mutex_progmemoria);
	programas_en_memoria++;
	pthread_mutex_unlock(&mutex_progmemoria);

	pcb_t *pcb_proceso = malloc(sizeof(pcb_t));
	bool rechazar = false;

	int base;

	pcb_proceso->pid = pid;
	base = crearSegmento(socketUmv, pcb_proceso->pid, strlen(codigoLiteral));
	if (base >= 0) { //Si se creo el segmento, envia y guarda.

		if (enviarDatos(socketUmv, base, 0, strlen(codigoLiteral), codigoLiteral) == 1) {

			pcb_proceso->codigo_segmento = base;
		}
		else {
			rechazar = true;

		}
	}
	else {
		rechazar = true;

	}

	if (metadataCodigo->cantidad_de_etiquetas > 0 || metadataCodigo->cantidad_de_funciones > 0) {
		base = crearSegmento(socketUmv, pcb_proceso->pid, metadataCodigo->etiquetas_size);

		if (base >= 0) { //Si se creo el segmento, envia y guarda.

			if (enviarDatos(socketUmv, base, 0, metadataCodigo->etiquetas_size, metadataCodigo->etiquetas) == 1) {

				pcb_proceso->indice_etiquetas = base;
			}
			else {
				rechazar = true;

			}

		}

		else {
			rechazar = true;

		}
	}

	base = crearSegmento(socketUmv, pcb_proceso->pid, metadataCodigo->instrucciones_size * 8);

	if (base >= 0) { //Si se creo el segmento, envia y guarda.

		if (enviarDatos(socketUmv, base, 0, metadataCodigo->instrucciones_size * 8, metadataCodigo->instrucciones_serializado) == 1) {
			pcb_proceso->indice_codigo = base;
		}
		else {

			rechazar = true;

		}

	}
	else {
		rechazar = true;

	}
	base = crearSegmento(socketUmv, pcb_proceso->pid, tamanioStack);
	if (base >= 0) { //Si se creo el segmento, envia y guarda.

		pcb_proceso->stack_segmento = base;
	}
	else {
		rechazar = true;

	}
	if (rechazar == true) {
		rechazarPrograma(socketPrograma, socketUmv, pcb_proceso->pid);

		return NULL ;
	}
	pcb_proceso->dir_contexto = 0;
	pcb_proceso->tamanio_indice_etiquetas = metadataCodigo->etiquetas_size;
	pcb_proceso->tamanio_contexto = 0;
	pcb_proceso->programCounter = metadataCodigo->instruccion_inicio;
	pcb_proceso->sock = socketPrograma;

	return pcb_proceso;
}
void rechazarPrograma(int socketPrograma, int socketUmv, int pid) {
	log_warning(logger, "Un programa ha sido rechazado P%d", pid);
	pthread_mutex_unlock(&mutex_new);
	sacarDeNew(socketPrograma);
	pthread_mutex_lock(&mutex_new);
	borrarPCB(socketUmv, pid);
	imprimirTextoEnProg("Rechazado", socketPrograma, sizeof("Rechazado"));
	enviarOrdenFinalizarProg(socketPrograma);



}

void pasarReady() {
	pthread_mutex_lock(&mutex_ready);
	pthread_mutex_lock(&mutex_new);
	bool imprimir = false;
	while (multiprogramacionDisponible() > 0 && list_size(cola_new) > 0) {
		colaNew_t *aux = list_get(cola_new, 0);
		pcb_t * auxPcb = malloc(sizeof(pcb_t));
		imprimir = true;
		auxPcb = crearPCB(aux->socketPrograma, socketUmv, aux->metadata, aux->codigo, aux->pid);
		if (auxPcb != NULL ) {
			log_info(logger, "PCB creado");
			free(aux);
			list_add(cola_ready, auxPcb);
			list_remove(cola_new, 0);
			log_info(logger, "Programa de new a ready");

		}
	}
	pthread_mutex_unlock(&mutex_ready);
	pthread_mutex_unlock(&mutex_new);
	if (imprimir) {
		imprimirColas();

	}

}
void enviaraCPU(int socket_cpu) {
	pthread_mutex_lock(&mutex_exec);
	pthread_mutex_lock(&mutex_ready);
	colaExec_t* prog = malloc(sizeof(colaExec_t));
	log_info(logger, "Se envia Programa a CPU");
	cabecera_t cabecera;
	cabecera.identificador = CPU_PCB;

	send(socket_cpu, &cabecera, sizeof(cabecera_t), 0);
	send(socket_cpu, list_get(cola_ready, 0), sizeof(pcb_t), 0);

	pcb_t* aux = list_get(cola_ready, 0);
	prog->pcb = *aux;
	prog->cpu = socket_cpu;
	list_add(cola_exec, prog);
	list_remove(cola_ready, 0);
	pthread_mutex_unlock(&mutex_exec);
	pthread_mutex_unlock(&mutex_ready);

}
void revisarCPUvacios() {
	pthread_mutex_lock(&mutex_cpuvacio);
	log_info(logger, "Revisa si hay CPU vacios y programas para ejecutar ");
	while (list_size(CPU_vacio) > 0 && tamanioReady() > 0) {

		int *num = list_get(CPU_vacio, 0);
		enviaraCPU(*num);
		list_remove(CPU_vacio, 0);

	}
	pthread_mutex_unlock(&mutex_cpuvacio);

}
int multiprogramacionDisponible() {

	return multiprogramacion - programas_en_memoria;
}
void imprimirColas() {
	pthread_mutex_lock(&mutex_block);
	pthread_mutex_lock(&mutex_ready);
	pthread_mutex_lock(&mutex_exec);
	pthread_mutex_lock(&mutex_new);
	pthread_mutex_lock(&mutex_exit);

	system("clear");
	puts("------Cola New------");
	colaNew_t *aux;
	int i;
	for (i = 0; i < list_size(cola_new); i++) {

		aux = list_get(cola_new, i);
		printf("P%d (%d)\n", aux->pid, aux->peso);

	}
	puts("--------------------");
	pcb_t *aux2;
	puts("------Cola Ready------");

	for (i = 0; i < list_size(cola_ready); i++) {

		aux2 = list_get(cola_ready, i);
		printf("P%d\n", aux2->pid);
	}
	puts("--------------------");
	colaBlock_t *aux3;
	puts("------Cola Block------");

	for (i = 0; i < list_size(cola_block); i++) {

		aux3 = list_get(cola_block, i);
		printf("P%d (%s)\n", aux3->pcb.pid, aux3->bloqueado);
	}
	puts("--------------------");
	colaExec_t *aux4;
	puts("------Cola Exec------");

	for (i = 0; i < list_size(cola_exec); i++) {

		aux4 = list_get(cola_exec, i);
		printf("P%d (%d)\n", aux4->pcb.pid, aux4->cpu);
	}
	puts("--------------------");

	puts("------Cola Exit------");
	int* aux5;
	for (i = 0; i < list_size(cola_exit); i++) {

		aux5 = list_get(cola_exit, i);
		printf("P%d\n", *aux5);
	}
	puts("--------------------");
	pthread_mutex_unlock(&mutex_block);
	pthread_mutex_unlock(&mutex_ready);
	pthread_mutex_unlock(&mutex_exec);
	pthread_mutex_unlock(&mutex_new);
	pthread_mutex_unlock(&mutex_exit);

}
int detectarColaDispositivo(char* dispositivo) {
	int i = 0;
	while (dispositivos_ES[i] != NULL ) {
		if (string_equals_ignore_case(dispositivos_ES[i], dispositivo)) {
			return i;
		}
		i++;
	}
	return -1;
}
int tamanioVectorChar(char **vec) {
	int i = 0;
	while (vec[i] != NULL ) {

		i++;
	}
	return i;
}

int obtenerPosicionEnArrayString(char* elemento, char** array) {
	int i = 0;
	while (i < tamanioVectorChar(array)) {
		if (string_equals_ignore_case(elemento, array[i])) {
			return i;
		}
		i++;
	}
	return -1;
}
int obtenerSockProgPorCpu(int cpu) {
	int i = 0;
	colaExec_t* aux;
	while (i < list_size(cola_exec)) {
		aux = list_get(cola_exec, i);
		if (aux->cpu == cpu) {

			return aux->pcb.sock;
		}
		i++;
	}
	return -1;
//free(aux);

}
int detectarIndiceProcesoBloqueado(pcb_t* pcb) {
	int i = 0;
	colaBlock_t* aux;

	while (i < list_size(cola_block)) {
		aux = list_get(cola_block, i);
		if (aux->pcb.pid == pcb->pid) {

			return i;
		}
		i++;
	}
	return -1;
}
int quitarDeColas(int socketProg, int socketUmv) {

	sacarDeNew(socketProg);

	int pid = sacarDeReady(socketProg);
	if (pid < 0) {
		pid = sacarDeBlock(socketProg); //Sacar de block implica sacar de las colas de semaforos y de es
		if (pid < 0) {
			pid = sacarDeExec(socketProg);

		}
	}

	return pid;
}
int sacarDeNew(int socketProg) {
	pthread_mutex_lock(&mutex_new);
	int k;
	colaNew_t *aux = malloc(sizeof(colaNew_t));
	for (k = 0; k < list_size(cola_new); k++) {
		aux = list_get(cola_new, k);

		if (aux->socketPrograma == socketProg) {
			list_remove(cola_new, k);
			k = aux->pid;
			free(aux);
			pthread_mutex_unlock(&mutex_new);
			return k;
		}

	}
	pthread_mutex_unlock(&mutex_new);
	//free(aux);
	return -1;
}
int sacarDeReady(int socketProg) {
	pthread_mutex_lock(&mutex_ready);
	int k;
	pcb_t *aux = malloc(sizeof(pcb_t));
	for (k = 0; k < list_size(cola_ready); k++) {
		aux = list_get(cola_ready, k);

		if (aux->sock == socketProg) {
			list_remove(cola_ready, k);
			k = aux->pid;
			free(aux);
			pthread_mutex_unlock(&mutex_ready);
			return k;
		}

	}
	pthread_mutex_unlock(&mutex_ready);
	return -1;
}
int sacarDeBlock(int socketProg) {
	pthread_mutex_lock(&mutex_block);
	int k, pid = -1;
	colaBlock_t *aux = malloc(sizeof(colaBlock_t));
	for (k = 0; k < list_size(cola_block); k++) {
		aux = list_get(cola_block, k);

		if (aux->pcb.sock == socketProg) {
			list_remove(cola_block, k);
			pid = aux->pcb.pid;
			//free(aux);
		}
	}
	if (pid > 0) {
		sacarDeES(pid);
		sacarDeSEM(pid);
	}
	pthread_mutex_unlock(&mutex_block);
	return pid;

}
int sacarDeExec(int socketProg) {
	pthread_mutex_lock(&mutex_exec);
	int k;
	colaExec_t *aux = malloc(sizeof(colaExec_t));
	for (k = 0; k < list_size(cola_exec); k++) {
		aux = list_get(cola_exec, k);

		if (aux->pcb.sock == socketProg) {
			list_remove(cola_exec, k);
			k = aux->pcb.pid;

			free(aux);
			pthread_mutex_unlock(&mutex_exec);
			return k;
		}

	}
	pthread_mutex_unlock(&mutex_exec);
	return -1;
}
bool estaEnExit(int pid) {
	pthread_mutex_lock(&mutex_exit);
	int k;
	int *aux = malloc(sizeof(int));
	for (k = 0; k < list_size(cola_exit); k++) {
		aux = list_get(cola_exit, k);

		if (*aux == pid) {
			pthread_mutex_unlock(&mutex_exit);
			return true;
		}

	}
	pthread_mutex_unlock(&mutex_exit);
	return false;
}
void sacarDeES(int pid) {
	pthread_mutex_lock(&mutex_es);
	int i, j;
	t_list* auxi;
	procesoES_t* auxj;
	for (i = 0; i < list_size(cola_es); i++) {
		auxi = list_get(cola_es, i);
		for (j = 0; j < list_size(auxi); j++) {
			auxj = list_get(auxi, j);
			if (auxj->pcb.pid == pid) {
				list_remove(auxi, j);
				j--; //Resta porque cuando hace remove, se acorta la lista.
			}
		}
	}
	pthread_mutex_unlock(&mutex_es);
}
void sacarDeSEM(int pid) {
	pthread_mutex_lock(&mutex_sem);
	int i, j;
	t_list* auxi;
	pcb_t* auxj;
	for (i = 0; i < list_size(cola_sem); i++) {
		auxi = list_get(cola_sem, i);
		for (j = 0; j < list_size(auxi); j++) {
			auxj = list_get(auxi, j);
			if (auxj->pid == pid) {
				list_remove(auxi, j);
				j--; //Resta porque cuando hace remove, se acorta la lista.
			}
		}
	}
	pthread_mutex_unlock(&mutex_sem);
}
void sacarCPUDeVacios(int cpu) {
	pthread_mutex_lock(&mutex_cpuvacio);
	int i;
	int* aux;
	for (i = 0; i < list_size(CPU_vacio); i++) {
		aux = list_get(CPU_vacio, i);
		if (*aux == cpu) {
			list_remove(CPU_vacio, i);
			pthread_mutex_unlock(&mutex_cpuvacio);
			return;
		}

	}
	pthread_mutex_unlock(&mutex_cpuvacio);
}
pcb_t* cortarEjecucion(int cpu) {
	log_info(logger, "Corta ejecucion actual de un CPU");
	cabecera_t* cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = CPU_Interrumpir;
	send(cpu, cabecera, sizeof(cabecera_t), 0);
	pcb_t* pcb = malloc(sizeof(pcb_t));
	int status = recv(cpu, pcb, sizeof(pcb_t), 0);

	if (status > 0)
		return pcb;
	else
		return NULL ;

}
void imprimirTextoEnProg(char* texto, int sockPrograma, int tamanio) {
	log_info(logger, "Se imprime texto en Programa");
	cabecera_t* cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = Programa_ImprimirTexto;
	send(sockPrograma, cabecera, sizeof(cabecera_t), 0);

	send(sockPrograma, &tamanio, sizeof(int), 0);

	send(sockPrograma, texto, tamanio, 0);

	free(cabecera);

}
void enviarOrdenFinalizarProg(int sockPrograma) {
	cabecera_t* cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = Programa_Finalizar;
	send(sockPrograma, cabecera, sizeof(cabecera_t), 0);
	free(cabecera);
}
void agregarCpuVacio(int sockCpu) {
	pthread_mutex_lock(&mutex_cpuvacio);
	log_info(logger, "Nuevo cpu vacio");
	int* aux_sockCpu = malloc(sizeof(int));
	*aux_sockCpu = sockCpu;
	list_add(CPU_vacio, aux_sockCpu);
	pthread_mutex_unlock(&mutex_cpuvacio);

}
int tamanioReady() {
	pthread_mutex_lock(&mutex_ready);
	int tam = list_size(cola_ready);
	pthread_mutex_unlock(&mutex_ready);
	return tam;
}
void agregarAExit(int pid) {
	int* aux_pid = malloc(sizeof(int));
	*aux_pid = pid;
	pthread_mutex_lock(&mutex_exit);
	list_add(cola_exit, aux_pid);
	pthread_mutex_unlock(&mutex_exit);
}

