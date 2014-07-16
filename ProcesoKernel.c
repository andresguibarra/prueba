
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
int tamanioStack; //ESTO TIENE QUE ENTRAR POR CONFIG

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

typedef struct parametro_hilo {
	int listenningSocket;
	int socketUmv;

} param_hilo;

typedef struct parametro_hiloES {
	char *dispositivo;
	int retardoDisp;
	int cola;
} param_hiloES;
typedef struct proceso_colaNew {
	pcb_t pcb;
	int peso;

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
pcb_t *crearPCB(int socketPrograma, int socketUmv, t_metadata_program *metadataCodigo, char *codigoLiteral);
void rechazarPrograma();
void borrarPCB(int socketUmv, int pid);
void pasarReady();
void enviaraCPU(int socket_cpu);
void revisarCPUvacios();
void imprimirColas();
int lugaresProgramasPCP();
int detectarColaDispositivo(char* dispositivo);
int tamanioVectorChar(char **vec);
int obtenerPosicionEnArrayString(char* elemento, char** array);
int obtenerSockProgPorCpu(int cpu);
int detectarIndiceProcesoBloqueado(pcb_t* pcb);
pcb_t cortarEjecucion(int cpu);
void quitarDeColas(int socketProg, int socketUmv);
int sacarDeNew(int socketProg);
int sacarDeReady(int socketProg);
int sacarDeBlock(int socketProg);
int sacarDeExec(int socketProg);
int buscarEnExit(int socketProg);
void sacarDeES(int pid);
void sacarDeSEM(int pid);
void sacarCPUDeVacios(int cpu);
void imprimirTextoEnProg(char* texto, int socketPrograma, int tamanio);
void enviarOrdenFinalizarProg(int socketPrograma);
void agregarCpuVacio(int sockCpu);

t_metadata_program *metadataCodigo;
pcb_t *pcb_proceso;
colaNew_t *procesoEntrante;
unsigned int pidAux = 0;
t_list *cola_new;
t_list *cola_ready;
t_list *cola_block;
t_list *cola_exit;
t_list *cola_exec;
t_list *cola_sem; //lista de colas de semaforos
t_list *CPU_vacio;
t_list *cola_es; //Lista de colas de I/O

int main(int argc, char **argv) {
	configurarKernel(argv);

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
	i = 0;
	while (semaforos[i] != NULL ) {
		t_list *aux;
		aux = list_create();
		list_add(cola_sem, aux);
		i++;
	}

	struct addrinfo hintsProg;
	struct addrinfo *serverInfoProg;

	struct addrinfo hintsUmv;
	struct addrinfo *serverInfoUmv;

	struct addrinfo hintsCpu;
	struct addrinfo *serverInfoCpu;

	memset(&hintsProg, 0, sizeof(hintsProg));
	hintsProg.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hintsProg.ai_flags = AI_PASSIVE;		// Asigna el address del localhost: 127.0.0.1
	hintsProg.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP
	getaddrinfo(NULL, puertoProg, &hintsProg, &serverInfoProg);

	memset(&hintsUmv, 0, sizeof(hintsUmv));
	hintsUmv.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hintsUmv.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP
	getaddrinfo(ipUmv, puertoUmv, &hintsUmv, &serverInfoUmv);

	memset(&hintsCpu, 0, sizeof(hintsCpu));
	hintsCpu.ai_family = AF_UNSPEC;		// No importa si uso IPv4 o IPv6
	hintsCpu.ai_flags = AI_PASSIVE;		// Asigna el address del localhost: 127.0.0.1
	hintsCpu.ai_socktype = SOCK_STREAM;	// Indica que usaremos el protocolo TCP
	getaddrinfo(NULL, puertoCpu, &hintsCpu, &serverInfoCpu);

	/*	Mediante socket(), obtengo el File Descriptor que me proporciona el sistema (un integer identificador)*/
	/* Obtenemos el socket que escuche las conexiones entrantes */

	int listenningSocketProg = socket(serverInfoProg->ai_family, serverInfoProg->ai_socktype, serverInfoProg->ai_protocol);
	int socketUmv = socket(serverInfoUmv->ai_family, serverInfoUmv->ai_socktype, serverInfoUmv->ai_protocol);
	int listenningSocketCpu = socket(serverInfoCpu->ai_family, serverInfoCpu->ai_socktype, serverInfoCpu->ai_protocol);

	/*le digo por que "telefono" tiene que esperar las llamadas.*/
	bind(listenningSocketProg, serverInfoProg->ai_addr, serverInfoProg->ai_addrlen);
	bind(listenningSocketCpu, serverInfoCpu->ai_addr, serverInfoCpu->ai_addrlen);
	connect(socketUmv, serverInfoUmv->ai_addr, serverInfoUmv->ai_addrlen);

	freeaddrinfo(serverInfoProg);
	freeaddrinfo(serverInfoUmv);
	freeaddrinfo(serverInfoCpu); // Ya no los vamos a necesitar

	param_hilo infoCpu;
	infoCpu.listenningSocket = listenningSocketCpu;
	infoCpu.socketUmv = socketUmv;
	param_hilo infoProg;
	infoProg.listenningSocket = listenningSocketProg;
	infoProg.socketUmv = socketUmv;

	pthread_t idHiloCpu;
	pthread_t idHiloProg;

	pthread_create(&idHiloCpu, NULL, pcp, (void *) &infoCpu);
	pthread_create(&idHiloProg, NULL, plp, (void *) &infoProg);

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
	param_hilo *info = (param_hilo *) param;

	fd_set active_fd_set, read_fd_set;
	int i;
	struct sockaddr_in clientname;
	socklen_t size;

	listen(info->listenningSocket, BACKLOG);

	FD_ZERO(&active_fd_set);
	FD_SET(info->listenningSocket, &active_fd_set);

	int escuchar = 1;
	while (escuchar) {

		FD_ZERO(&read_fd_set);
		read_fd_set = active_fd_set;

		if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL ) < 0) {
			perror("select");
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < FD_SETSIZE; ++i) { // FD_SETSIZE vale 1024 por defecto

			if (FD_ISSET (i, &read_fd_set)) { //FD_ISSET Retorna True si i (socket) esta en el set.

				if (i == info->listenningSocket) { //Compara si i es el socket por el que estaba escuchando
					//Y si es, escuchamos la nueva conexion y lo agregamos a la lista

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

					cabecera_t* cabecera = malloc(sizeof(cabecera_t));

					int status = recv(i, cabecera, sizeof(cabecera_t), 0);

					switch (cabecera->identificador) {

					case Kernel_Request: {
						cabecera->identificador = Programa_OK;
						send(i, cabecera, sizeof(cabecera_t), 0);
						break;
					}

					}
					free(cabecera);

					if (status != 0) {
						int tamanio;

						status = recv(i, &tamanio, 4, 0); //Recibe el tamaño
						char *package = malloc(PACKAGESIZE);
						status = recv(i, (void*) package, tamanio, 0); //Recibe el codigo literal

						metadataCodigo = malloc(sizeof(t_metadata_program));
						pcb_proceso = malloc(sizeof(pcb_t));
						procesoEntrante = malloc(sizeof(colaNew_t));
						colaNew_t *aux = malloc(sizeof(colaNew_t));

						metadataCodigo = metadata_desde_literal(package);

						pcb_proceso = crearPCB(i, info->socketUmv, metadataCodigo, package);

						if (pcb_proceso != NULL ) {

							procesoEntrante->pcb = *pcb_proceso;
							procesoEntrante->peso = 5 * metadataCodigo->cantidad_de_etiquetas + 3 * metadataCodigo->cantidad_de_funciones + metadataCodigo->instrucciones_size;

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

							imprimirColas();

						}
					}

					else { //Si el programa cerro la conexion, saca el socket de la lista y el proceso de la cola de new y de la umv.

						FD_CLR(i, &active_fd_set);

						quitarDeColas(i, info->socketUmv);

						imprimirColas();

					}
				}
			}

		}
		pasarReady();
		revisarCPUvacios();
		imprimirColas();

	}
	close(info->socketUmv);
	close(info->listenningSocket);
	free(pcb_proceso);
	free(metadataCodigo);
	free(procesoEntrante);
	//free(aux);

	return 0;
}

void *pcp(void *param) {

	param_hilo *info = (param_hilo *) param;
	CPU_vacio = list_create(); //Lista que tiene los socket de los CPU que estan al pedo.
	fd_set active_fd_set, read_fd_set;
	int i;
	struct sockaddr_in clientname;
	socklen_t size;

	listen(info->listenningSocket, BACKLOG);

	FD_ZERO(&active_fd_set);
	FD_SET(info->listenningSocket, &active_fd_set);

	int escuchar = 1;
	while (escuchar) {

		FD_ZERO(&read_fd_set);
		read_fd_set = active_fd_set;

		if (select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL ) < 0) {

			perror("select");
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < FD_SETSIZE; ++i) // FD_SETSIZE vale 1024 por defecto
			if (FD_ISSET (i, &read_fd_set)) { //FD_ISSET Retorna True si i (socket) esta en el set.

				if (i == info->listenningSocket) { //Compara si i es el socket por el que estaba escuchando
					//Y si es, escuchamos la nueva conexion y lo agregamos a la lista

					int new;
					size = sizeof(clientname);
					new = accept(i, (struct sockaddr *) &clientname, &size);
					if (new < 0) {
						perror("accept");
						exit(EXIT_FAILURE);
					}

					printf("Un CPU se acaba de conectar, port %d.\n", ntohs(clientname.sin_port));
					FD_SET(new, &active_fd_set);
					int* auxNew = malloc(sizeof(int));
					*auxNew = new;
					list_add(CPU_vacio, auxNew);
					send(new, &quantum, sizeof(quantum), 0);
					send(new, &retardo, sizeof(retardo), 0);
				}
				else {
					//SINO ES EL SOCKET POR EL QUE ESTAMOS ESCUCHANDO, RECIBE.

					cabecera_t cabecera;

					int status = recv(i, &cabecera, sizeof(cabecera_t), 0); //Recibe la cabecera

					switch (cabecera.identificador) {
					case Quantum_cumplido:
						puts("RECIBE QUANTUM CUMPLIDO");

						if (list_size(cola_ready) > 0) {

							pcb_t *pcb_saliente = malloc(sizeof(pcb_t));

							sacarDeExec(obtenerSockProgPorCpu(i));
							puts("Antes de recibir el pcb saliente");

							puts("Despues de recibir el pcb slaiente");



							enviaraCPU(i); //Envia el nuevo PCB

							recv(i, pcb_saliente, sizeof(pcb_t), 0);
							list_add(cola_ready, pcb_saliente);
							imprimirColas();

						}
						else {

							cabecera.identificador = CPU_Continuar;
							send(i, &cabecera, sizeof(cabecera_t), 0);
							puts("Envia continuar");
						}
						break;

					case Id_ProgramaFinalizado:
						if (status > 0) {

							pcb_t *pcb_saliente = malloc(sizeof(pcb_t));
							recv(i, pcb_saliente, sizeof(pcb_t), 0);

							enviarOrdenFinalizarProg(pcb_saliente->sock);
							sacarDeExec(pcb_saliente->sock);
							borrarPCB(info->socketUmv, pcb_saliente->pid);
							list_add(cola_exit, pcb_saliente);

							if (list_size(cola_ready) > 0) {

								enviaraCPU(i); //Envia el nuevo PCB e imprime colas

							}
							else {
								agregarCpuVacio(i);

							}

						}
						break;

					case Kernel_ObtenerValorCompartida: {
						int tamanio, pos;
						recv(i, &tamanio, sizeof(int), 0);
						char*variable = malloc(tamanio);
						recv(i, variable, tamanio, 0);
						pos = obtenerPosicionEnArrayString(variable, variables_globales);
						send(i, &valores_globales[pos], sizeof(int), 0);
						free(variable);
						break;
					}
					case Kernel_AsignarValorCompartida: {
						int tamanio, pos, valor;
						recv(i, &tamanio, sizeof(int), 0);
						char* variable = malloc(tamanio);
						recv(i, variable, tamanio, 0);
						recv(i, &valor, sizeof(int), 0);
						pos = obtenerPosicionEnArrayString(variable, variables_globales);
						valores_globales[pos] = valor;
						free(variable);
						break;
					}
					case Kernel_Imprimir: {

						t_valor_variable *valor_variable = malloc(sizeof(t_valor_variable));
						recv(i, valor_variable, sizeof(t_valor_variable), 0);
						int *sockPrograma = malloc(sizeof(int));
						recv(i, sockPrograma, sizeof(int), 0);

						cabecera_t* cabecera = malloc(sizeof(cabecera_t));
						cabecera->identificador = Programa_Imprimir;
						send(*sockPrograma, cabecera, sizeof(cabecera_t), 0);
						send(*sockPrograma, valor_variable, sizeof(int), 0);

						free(sockPrograma);
						free(cabecera);
						break;
					}
					case Kernel_ImprimirTexto: {

						int *tamanio = malloc(sizeof(int));
						int *sockPrograma = malloc(sizeof(int));
						recv(i, tamanio, sizeof(int), 0);
						void *buffer = malloc(*tamanio);
						recv(i, sockPrograma, sizeof(int), 0);
						recv(i, buffer, *tamanio, 0);
						((char *) buffer)[*tamanio - 1] = 0;

						imprimirTextoEnProg(buffer, *sockPrograma, *tamanio);
						free(sockPrograma);
						free(tamanio);

						break;
					}
					case Kernel_EntradaSalida: {

						pcb_t *pcb_saliente = malloc(sizeof(pcb_t));
						procesoES_t* procesoES = malloc(sizeof(procesoES_t));
						int tamanioTipoES;
						int instantes;
						recv(i, pcb_saliente, sizeof(pcb_t), 0);
						recv(i, &tamanioTipoES, sizeof(int), 0);

						char *tipoES = malloc(tamanioTipoES);
						recv(i, tipoES, tamanioTipoES, 0);
						recv(i, &instantes, sizeof(int), 0);
						procesoES->pcb = *pcb_saliente;
						procesoES->instantes = instantes;

						int disp = detectarColaDispositivo(tipoES);

						sacarDeExec(obtenerSockProgPorCpu(i));

						colaBlock_t* procesoBlock = malloc(sizeof(colaBlock_t));

						procesoBlock->pcb = *pcb_saliente;

						procesoBlock->bloqueado = tipoES;

						list_add(cola_block, procesoBlock);

						list_add(list_get(cola_es, disp), procesoES);

						if (list_size(cola_ready) > 0) {

							enviaraCPU(i); //Envia el nuevo PCB e imprime colas


						}
						else {
							agregarCpuVacio(i);

						}
						imprimirColas();
						free(pcb_saliente);
						free(tipoES);
						break;
					}
					case Kernel_Signal: {
						int tamanio, pos;
						recv(i, &tamanio, sizeof(int), 0);
						char* sem = malloc(tamanio);
						recv(i, sem, tamanio, 0);
						printf("%s \n", sem);

						pos = obtenerPosicionEnArrayString(sem, semaforos);
						free(sem);
						valores_semaforos[pos]++;
						printf("SIGNAL semaforo %s : %d \n", semaforos[pos], valores_semaforos[pos]);
						if (valores_semaforos[pos] <= 0) {
							list_add(cola_ready, list_get(list_get(cola_sem, pos), 0));
							list_remove(cola_block, detectarIndiceProcesoBloqueado(list_get(list_get(cola_sem, pos), 0)));
							list_remove(list_get(cola_sem, pos), 0);
							imprimirColas();
						}

						break;
					}
					case Kernel_Wait: {
						int tamanio, pos;
						recv(i, &tamanio, sizeof(int), 0);
						char* sem = malloc(tamanio);
						recv(i, sem, tamanio, 0);
						puts("asd");
						pos = obtenerPosicionEnArrayString(sem, semaforos);
						printf("%s \n", sem);
						free(sem);

						valores_semaforos[pos]--;
						printf("WAIT semaforo %s : %d \n", semaforos[pos], valores_semaforos[pos]);
						if (valores_semaforos[pos] < 0) { //Si se bloquea

							sacarDeExec(obtenerSockProgPorCpu(i));
							colaBlock_t* procBloqueado = malloc(sizeof(colaBlock_t));
							procBloqueado->pcb = cortarEjecucion(i);
							procBloqueado->bloqueado = semaforos[pos];
							agregarCpuVacio(i);
							list_add(cola_block, procBloqueado);
							list_add(list_get(cola_sem, pos), &procBloqueado->pcb);
							imprimirColas();
						}
						else {
							cabecera_t* cabecera = malloc(sizeof(cabecera_t));
							cabecera->identificador = CPU_Continuar;
							send(i, cabecera, sizeof(cabecera_t), 0);

						}

						break;
					}
					case CPU_Apagado: {

						break;
					}

					}

					if (status > 0) {

					}
					else if (status == 0) {
						//Si el cpu cerro la conexion, saca el socket de la lista.
						FD_CLR(i, &active_fd_set);
						sacarCPUDeVacios(i);
						int socketProg = obtenerSockProgPorCpu(i);
						if (socketProg > 0) {
							imprimirTextoEnProg("****Cpu desconectado durante ejecucion****", socketProg, sizeof("****Cpu desconectado durante ejecucion****"));

							enviarOrdenFinalizarProg(socketProg);
						}
					}

				}
			}

		pasarReady();
		revisarCPUvacios();
		imprimirColas();

	}
	close(info->listenningSocket);
	return 0;
}

void *HIO(void *param) {

	param_hiloES *info = (param_hiloES *) param;

	while (true) {
		while (list_size(list_get(cola_es, info->cola)) > 0) {

			procesoES_t* aux = list_get(list_get(cola_es, info->cola), 0);

			usleep(info->retardoDisp * 1000 * aux->instantes); // *1000 porque usleep trabaja en microsegundos. y el retardo esta en milisegundos.

			list_add(cola_ready, &(aux->pcb)); //Manda el pcb a ready devuelta

			list_remove(list_get(cola_es, info->cola), 0);

			list_remove(cola_block, detectarIndiceProcesoBloqueado(&(aux->pcb)));

			imprimirColas();
			revisarCPUvacios();
			imprimirColas();

		}

	}

	return 0;
}

void borrarPCB(int socketUmv, int pid) {

	cabecera_t cabecera;
	destruirsegmento_t *segmento = malloc(sizeof(destruirsegmento_t));
	cabecera.identificador = IdDestruirSegmento;
	send(socketUmv, &cabecera, sizeof(cabecera), 0);
	segmento->idPCB = pid;
	send(socketUmv, segmento, sizeof(destruirsegmento_t), 0); //Envia datos del pcb a eliminar.
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
	recv(socketUmv, (void*) respuesta, sizeof(int), 0);
	return *respuesta;
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

	recv(sock, &ok, sizeof(ok), 0);

	return ok;
}

pcb_t *crearPCB(int socketPrograma, int socketUmv, t_metadata_program *metadataCodigo, char *codigoLiteral) {

	cabecera_t cabecera;
	idproceso_t idProceso;
	pcb_t *pcb_proceso = malloc(sizeof(pcb_t));
	bool rechazar = false;

	strcpy(idProceso.tipo, "kernel");

	int base;

	cabecera.identificador = IdHandshake;
	send(socketUmv, &cabecera, sizeof(cabecera), 0);
	send(socketUmv, &idProceso, sizeof(idproceso_t), 0);

	pidAux++;
	pcb_proceso->pid = pidAux;
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
		pidAux--;
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
	borrarPCB(socketUmv, pid);
	imprimirTextoEnProg("Rechazado", socketPrograma, sizeof("Rechazado"));
	enviarOrdenFinalizarProg(socketPrograma);

}

void pasarReady() {
	bool imprimir = false;
	while (lugaresProgramasPCP() > 0 && list_size(cola_new) > 0) {
		colaNew_t *aux = list_get(cola_new, 0);
		pcb_t * auxPcb = malloc(sizeof(pcb_t));
		*auxPcb = aux->pcb;
		free(aux);
		list_add(cola_ready, auxPcb);
		list_remove(cola_new, 0);
		imprimir = true;
	}
	if (imprimir) {
		imprimirColas();

	}
}
void enviaraCPU(int socket_cpu) {
	colaExec_t* prog = malloc(sizeof(colaExec_t));

	cabecera_t cabecera;
	cabecera.identificador = CPU_PCB;

	send(socket_cpu, &cabecera, sizeof(cabecera_t), 0);
	send(socket_cpu, list_get(cola_ready, 0), sizeof(pcb_t), 0);

	pcb_t* aux = list_get(cola_ready, 0);
	prog->pcb = *aux;
	prog->cpu = socket_cpu;
	list_add(cola_exec, prog);
	list_remove(cola_ready, 0);

}
void revisarCPUvacios() {

	while (list_size(CPU_vacio) > 0 && list_size(cola_ready) > 0) {

		int *num = list_get(CPU_vacio, 0);
		enviaraCPU(*num);
		list_remove(CPU_vacio, 0);

	}

}
int lugaresProgramasPCP() {

	return multiprogramacion - list_size(cola_ready);
}
void imprimirColas() {
	system("clear");
	puts("------Cola New------");
	colaNew_t *aux;
	int i;
	for (i = 0; i < list_size(cola_new); i++) {

		aux = list_get(cola_new, i);
		printf("P%d (%d)\n", aux->pcb.pid, aux->peso);

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

	for (i = 0; i < list_size(cola_exit); i++) {

		aux2 = list_get(cola_exit, i);
		printf("P%d\n", aux2->pid);
	}
	puts("--------------------");

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
void quitarDeColas(int socketProg, int socketUmv) {

	int pid = sacarDeNew(socketProg);
	if (pid < 0) {
		pid = sacarDeReady(socketProg);
		if (pid < 0) {
			pid = sacarDeBlock(socketProg); //Sacar de block implica sacar de las colas de semaforos y de es
			if (pid < 0) {
				pid = sacarDeExec(socketProg);
				if (pid < 0) {
					pid = buscarEnExit(socketProg);
				}
			}
		}

	}
	printf("Cerrado P%d \n", pid);
	if (pid > 0) {
		borrarPCB(socketUmv, pid);
	}
}
int sacarDeNew(int socketProg) {

	int k;
	colaNew_t *aux = malloc(sizeof(colaNew_t));
	for (k = 0; k < list_size(cola_new); k++) {
		aux = list_get(cola_new, k);

		if (aux->pcb.sock == socketProg) {
			list_remove(cola_new, k);
			return aux->pcb.pid;
		}

	}
	//free(aux);
	return -1;
}
int sacarDeReady(int socketProg) {
	int k;
	pcb_t *aux = malloc(sizeof(pcb_t));
	for (k = 0; k < list_size(cola_ready); k++) {
		aux = list_get(cola_ready, k);

		if (aux->sock == socketProg) {
			list_remove(cola_ready, k);
			return aux->pid;
		}

	}
	//free(aux);  SI METO ESTE FREE, CUANDO METO 1 PROGRAMA Y DESPUES METO OTRO QUE LO RECHAZA, EL QUE ESTABA EN READY FLASHEA MIERDA Y QUEDA EN P0
	return -1;
}
int sacarDeBlock(int socketProg) {
	int k, pid = -1;
	colaBlock_t *aux = malloc(sizeof(colaBlock_t));
	for (k = 0; k < list_size(cola_block); k++) {
		aux = list_get(cola_block, k);

		if (aux->pcb.sock == socketProg) {
			list_remove(cola_block, k);
			pid = aux->pcb.pid;
		}
	}
	if (pid > 0) {
		sacarDeES(pid);
		sacarDeSEM(pid);
	}
	//free(aux);
	return pid;

}
int sacarDeExec(int socketProg) {
	int k;
	colaExec_t *aux = malloc(sizeof(colaExec_t));
	for (k = 0; k < list_size(cola_exec); k++) {
		aux = list_get(cola_exec, k);

		if (aux->pcb.sock == socketProg) {
			list_remove(cola_exec, k);
			return aux->pcb.pid;
		}

	}
	//free(aux);
	return -1;
}
int buscarEnExit(int socketProg) {
	int k;
	pcb_t *aux = malloc(sizeof(pcb_t));
	for (k = 0; k < list_size(cola_exit); k++) {
		aux = list_get(cola_exit, k);

		if (aux->sock == socketProg) {

			return aux->pid;
		}

	}
	//free(aux);
	return -1;
}
void sacarDeES(int pid) {
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
}
void sacarDeSEM(int pid) {
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
}
void sacarCPUDeVacios(int cpu) {

	int i;
	int* aux;
	for (i = 0; i < list_size(CPU_vacio); i++) {
		aux = list_get(CPU_vacio, i);
		if (*aux == cpu) {
			list_remove(CPU_vacio, i);
			return;
		}

	}
}
pcb_t cortarEjecucion(int cpu) {
	cabecera_t* cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = CPU_Interrumpir;
	send(cpu, cabecera, sizeof(cabecera_t), 0);
	pcb_t pcb;
	recv(cpu, &pcb, sizeof(pcb_t), 0);
	return pcb;
}
void imprimirTextoEnProg(char* texto, int sockPrograma, int tamanio) {
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
	int* aux_sockCpu = malloc(sizeof(int));
	*aux_sockCpu = sockCpu;
	list_add(CPU_vacio, aux_sockCpu);

}
