/*
 ============================================================================
 Name        : ProcesoCPU.c
 Author      : Andres Guibarra & Nicolas Mico
 Version     :
 Copyright   : Your copyright notice
 Description : Proceso que simula el funcionamiento de un CPU para el tabajo practico de Sistemas Operativos
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <commons/string.h>
#include <commons/error.h>
#include <commons/config.h>
#include <commons/bitarray.h>
#include <commons/collections/list.h>
#include <commons/log.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <parser/parser.h>
#include <parser/metadata_program.h>
#include "ProcesoCPU.h"
#include "serial.h"

void iniciarCPU(char **cadena);
void *conectar_kernel();
void esperar_seniales(int n);
int recibirQuantum(int sock);
void solicitudTonta();
void conectar_umv();
void avisoQuantumCumplido();
void finalizarEjecucion();
void finalizarPrograma();
void solicitarInstruccion(int sock);
void regenerar_contexto();
void preservarContexto(t_nombre_etiqueta etiqueta, t_puntero *retorno);
int solicitarDatos(int sock, int base, int offset, int tamanio, void *buffer);
int enviarDatos(int sock, int base, int offset, int tamanio, void *buffer);
void obtenerValoresConfiguracion(char *VARIABLE, char *expected, int tamanio, t_config *config);
int make_socket(char *PORT, char *IP);
void push(t_list *list, void *element);
void enviarhandshake(int socket, char *tipoProceso);
pcb_t *recibirPCB(int sock);
t_log *logger;
char *fileName = "log_CPU";
char *IP_KERNEL, *PUERTO_KERNEL, *IP_UMV, *PUERTO_UMV, *CONSOLA_ACTIVA, *LOG_LEVEL;
int umvSocket, kernelSocket, QUANTUM, QUANTUM_RETARDO;
bool finalizado;
bool recibiSignalKill;
t_list *diccionarioVariables;

pcb_t *pcbGlobal;

AnSISOP_funciones functions = { .AnSISOP_irAlLabel = _irAlLabel,

.AnSISOP_definirVariable = _definirVariable,

.AnSISOP_obtenerPosicionVariable = _obtenerPosicionVariable,

.AnSISOP_dereferenciar = _dereferenciar,

.AnSISOP_asignar = _asignar,

.AnSISOP_finalizar = _finalizar,

.AnSISOP_asignarValorCompartida = _asignarValorCompartida,

.AnSISOP_obtenerValorCompartida = _obtenerValorCompartida,

.AnSISOP_llamarConRetorno = _llamarConRetorno,

.AnSISOP_llamarSinRetorno = _llamarSinRetorno,

.AnSISOP_retornar = _retornar,

.AnSISOP_imprimir = _imprimir,

.AnSISOP_imprimirTexto = _imprimirTexto,

.AnSISOP_entradaSalida = _entradaSalida };

AnSISOP_kernel kernel_functions = {

.AnSISOP_wait = _wait,

.AnSISOP_signal = _signal };
void esperar_seniales(int n) {

	switch (n) {
		case SIGUSR1: {
			recibiSignalKill = true;
			break;
		}
	}
}

void finalizarEjecucion() {
	log_trace(logger, "Envio PCB al KERNEL");
	cabecera_t *cabecera = malloc(sizeof(pcb_t));
	cabecera->identificador = CPU_Apagado;
	send(kernelSocket, (void *) cabecera, sizeof(cabecera_t), 0);
	send(kernelSocket, (void *) pcbGlobal, sizeof(pcb_t), 0);
	close(kernelSocket);
	close(umvSocket);
	exit(EXIT_SUCCESS);

}
int main(int argc, char **argv) {
	iniciarCPU(argv);
	pthread_t hiloSockets;
	signal(SIGUSR1, esperar_seniales);
	pthread_create(&hiloSockets, NULL, conectar_kernel, NULL );
	pthread_join(hiloSockets, NULL );

	return 0;
}
void iniciarCPU(char **cadena) {
	diccionarioVariables = list_create();
	t_config *miConfig = config_create(cadena[1]);
	IP_KERNEL = malloc(16);
	IP_UMV = malloc(16);
	PUERTO_KERNEL = malloc(6);
	PUERTO_UMV = malloc(6);
	CONSOLA_ACTIVA = malloc(6);
	LOG_LEVEL = malloc(20);
	obtenerValoresConfiguracion(IP_KERNEL, "IP_KERNEL", 16, miConfig);
	obtenerValoresConfiguracion(IP_UMV, "IP_UMV", 16, miConfig);
	obtenerValoresConfiguracion(PUERTO_KERNEL, "PUERTO_KERNEL", 6, miConfig);
	obtenerValoresConfiguracion(PUERTO_UMV, "PUERTO_UMV", 6, miConfig);
	obtenerValoresConfiguracion(CONSOLA_ACTIVA, "CONSOLA_ACTIVA", 6, miConfig);
	obtenerValoresConfiguracion(LOG_LEVEL, "LOG_LEVEL", 20, miConfig);
	config_destroy(miConfig);
	if (string_equals_ignore_case(CONSOLA_ACTIVA, "true"))
		logger = log_create(fileName, "CPU", true, LOG_LEVEL_TRACE);
	else {
		logger = log_create(fileName, "CPU", false, LOG_LEVEL_TRACE);
	}
}
void obtenerValoresConfiguracion(char *VARIABLE, char *expected, int tamanio, t_config *config) {
	if (config_has_property(config, expected)) {

		memcpy(VARIABLE, config_get_string_value(config, expected), tamanio);

	} else {
		log_error(logger, "Se necesita %s de %s", string_split(expected, "_")[0], string_split(expected, "_")[1]);
		error_show("Se necesita %s de %s\n", string_split(expected, "_")[0], string_split(expected, "_")[1]);
	}
}

int make_socket(char *PORT, char *IP) {
	int sock;
	struct addrinfo *serverInfo;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	getaddrinfo(IP, PORT, &hints, &serverInfo);

	sock = socket(serverInfo->ai_family, serverInfo->ai_socktype, serverInfo->ai_protocol);
	connect(sock, serverInfo->ai_addr, serverInfo->ai_addrlen);
	freeaddrinfo(serverInfo);
	return sock;
}

void *conectar_kernel() {
	kernelSocket = make_socket(PUERTO_KERNEL, IP_KERNEL);
	log_trace(logger, "Se realizo conexion con KERNEL");
	conectar_umv();
	recibiSignalKill = false;
	QUANTUM = recibirQuantum(kernelSocket);
	QUANTUM_RETARDO = recibirQuantum(kernelSocket);
	log_trace(logger, "Se reciben Quantum: %d y Quantum_Retardo: %d", QUANTUM, QUANTUM_RETARDO);
	while (1) {
		cabecera_t *cabecera = malloc(sizeof(cabecera_t));
		recv(kernelSocket, (cabecera_t *) cabecera, sizeof(cabecera_t), 0);
		pcbGlobal = recibirPCB(kernelSocket);
		finalizado = false;
		regenerar_contexto();
		solicitarInstruccion(umvSocket);
		log_info(logger, "-----FIN DE UN PROCESO-----");

		free(cabecera);
	}

	return NULL ;
}
void avisoQuantumCumplido() { //1 send
	cabecera_t *cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = Quantum_cumplido;
	send(kernelSocket, cabecera, sizeof(cabecera_t), 0);

}
int recibirQuantum(int sock) { //1 recv
	int *quantum = malloc(sizeof(int));

	recv(sock, (int *) quantum, sizeof(int), 0);
	return *quantum;
}
void conectar_umv() {

	umvSocket = make_socket(PUERTO_UMV, IP_UMV);
	enviarhandshake(umvSocket, "cpu");
	log_trace(logger, "Se realizo conexion con UMV");
}

void solicitarInstruccion(int sock) {
	int contadorQuantum = 0;
	void *indice = malloc(2 * sizeof(int));
	while (finalizado != true && solicitarDatos(umvSocket, pcbGlobal->indice_codigo, pcbGlobal->programCounter * 8, 8, indice) != -1) {

		int offset, tamanio;
		memcpy(&offset, indice, 4);
		memcpy(&tamanio, indice + 4, 4);

		char *buffer = malloc(tamanio);
		log_debug(logger, "Pido instruccion a la UMV - Tamanio: %d Offset: %d", tamanio, offset);
		if (solicitarDatos(umvSocket, pcbGlobal->codigo_segmento, offset, tamanio, buffer) == -1) {
			error_show("Segmentation Fault\n");
			log_error(logger, "Segmentation Fault");

		}
		buffer[tamanio - 1] = 0;
		log_debug(logger, "El buffer es: %s", (char *) buffer);

		usleep(QUANTUM_RETARDO * 1000);
		analizadorLinea(buffer, &functions, &kernel_functions);
		pcbGlobal->programCounter++;
		contadorQuantum++;

		if (contadorQuantum == QUANTUM && finalizado != true) { //Una vez que concluyo el quantum le aviso al kernel
			log_debug(logger, "Se cumplio un Quantum");
			if (recibiSignalKill == true) {
				log_warning(logger, "Recibi SIGUSR1");
				finalizarEjecucion();
			}
			avisoQuantumCumplido();

			cabecera_t *cabecera = malloc(sizeof(cabecera_t));
			recv(kernelSocket, (cabecera_t *) cabecera, sizeof(cabecera_t), 0);

			switch (cabecera->identificador) {
				case CPU_Continuar: {
					contadorQuantum = 0;
					log_debug(logger, "Continuar Ejecucion");
					break;
				}
				case CPU_PCB: {
					log_debug(logger, "Context Switch");
					send(kernelSocket, pcbGlobal, sizeof(pcb_t), 0);
					pcbGlobal = recibirPCB(kernelSocket);
					regenerar_contexto();
					finalizado = false;
					contadorQuantum = 0;
					break;
				}
			}
			free(cabecera);
		}
		free(buffer);

	}
	free(indice);
}

void enviarhandshake(int sock, char *tipoProceso) {
	cabecera_t cabecera;
	cabecera.identificador = IdHandshake;
	idproceso_t idproceso;
	strcpy(idproceso.tipo, "cpu");
	send(sock, &cabecera, sizeof(cabecera_t), 0);
	send(sock, &idproceso, sizeof(idproceso_t), 0);
}

pcb_t *recibirPCB(int sock) {

	pcb_t *pcb = malloc(sizeof(pcb_t));

	recv(sock, (pcb_t *) pcb, sizeof(pcb_t), 0);

	/*printf("PID %d \n", pcb->pid);
	 printf("Codigo Semgento %d \n", pcb->codigo_segmento);
	 printf("Stack Base %d \n", pcb->stackBase);
	 printf("Cursor Contexto %d \n", pcb->cursorContexto);
	 printf("Indice Etiquetas %d \n", pcb->indice_etiquetas);
	 printf("Program Counter %d \n", pcb->programCounter);
	 printf("TamanioContextoActual %d \n", pcb->tamanio_contexto_actual);
	 printf("TamanioIndiceEtiquetas %d \n", pcb->indice_etiquetas);
	 printf("Indice Codigo %d \n", pcb->indice_codigo);
	 printf("Socket %d \n", pcb->sock);*/
	log_trace(logger, "Recibo PCB del kernel \nPID %d \nCursor Contexto %d \nProgram Counter %d \nTamanioContextoActual %d \nTamanioIndiceEtiquetas %d \n", pcb->pid, pcb->cursorContexto,
			pcb->programCounter, pcb->tamanio_contexto_actual, pcb->tamanio_indice_etiquetas);
	return pcb;
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

int solicitarDatos(int sock, int base, int offset, int tamanio, void *buffer) {
	bytes_t bytes;
	bytes.base = base;
	bytes.offset = offset;
	bytes.tamanio = tamanio;
	cabecera_t cabecera;
	cabecera.identificador = IdSolicitarBytes;

	send(sock, &cabecera, sizeof(cabecera_t), 0);
	send(sock, &bytes, sizeof(bytes_t), 0);
	int ok;
	void *bufferLocal = malloc(tamanio);
	recv(sock, &ok, sizeof(ok), 0);
	if (ok != -1) {
		recv(sock, (void *) bufferLocal, tamanio, 0);
		memcpy(buffer, bufferLocal, tamanio);
		free(bufferLocal);
		return ok;
	} else {
		return -1;
	}
}

t_puntero _definirVariable(t_nombre_variable variable) {
	if (finalizado == false) {
		log_trace(logger, "Definir Variable");
		if (enviarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto + (pcbGlobal->tamanio_contexto_actual * 5), 1, &variable) == 1) {

			variables_t *var = malloc(sizeof(variables_t));
			var->variable = variable;
			var->offset = pcbGlobal->cursorContexto + (pcbGlobal->tamanio_contexto_actual * 5) + 1;
			push(diccionarioVariables, var);
			pcbGlobal->tamanio_contexto_actual += 1;
			return var->offset;
		} else {
			error_show("Memory Overload\n");
			log_error(logger, "Memory Overload");
			finalizarPrograma();
			return -1;
		}
	}
	return 0;
}
t_puntero _obtenerPosicionVariable(t_nombre_variable identificador_variable) {

	log_trace(logger, "Obtener Posicion de Variable");

	int i;
	for (i = 0; i < list_size(diccionarioVariables); i++) {

		variables_t *var = (variables_t *) list_get(diccionarioVariables, i);
		if (var->variable == identificador_variable) {
			log_debug(logger, "Variable encontrada en el diccionario %c, offset %d", var->variable, var->offset);
			return var->offset;
		}

	}
	return -1;

}
void finalizarPrograma() {

	cabecera_t *cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = Id_ProgramaFinalizado;
	send(kernelSocket, cabecera, sizeof(cabecera_t), 0);
	send(kernelSocket, pcbGlobal, sizeof(pcb_t), 0);
	finalizado = true;
	log_debug(logger, "Se envia PCB");
	free(cabecera);

}
t_valor_variable _dereferenciar(t_puntero direccion_variable) {

	log_trace(logger, "Dereferenciar");
	t_valor_variable buffer;
	if (finalizado == false) {
		if (solicitarDatos(umvSocket, pcbGlobal->stackBase, direccion_variable, sizeof(int), &buffer) == -1) {
			error_show("Segmentation Fault\n");
			log_error(logger, "Segmentation Fault");
			finalizarPrograma();

		}
		log_debug(logger, "Direccion variable a dereferenciar %d Valor variable dereferenciada %d", direccion_variable, buffer); //puse cursorContexto
	}
	return buffer;
}

void _asignar(t_puntero direccion_variable, t_valor_variable valor) {
	log_trace(logger, "Asignar");
	log_debug(logger, "Direccion variable a asignar %d y valor variable a asignar %d ", direccion_variable, valor);
	if (finalizado == false) {
		if (direccion_variable == -1) {
			error_show("Memory Overload\n");
			log_error(logger, "Memory Overload");
			finalizarPrograma();
			return;
		}
		if (enviarDatos(umvSocket, pcbGlobal->stackBase, direccion_variable, sizeof(valor), &valor) == -1) {
			error_show("Memory Overload\n");
			log_error(logger, "Memory Overload");
			finalizarPrograma();
		}
	}
}

t_valor_variable _obtenerValorCompartida(t_nombre_compartida variable) {
	if (finalizado == false) {
		log_trace(logger, "Kernel obtenerValorCompartida");
		cabecera_t *cabecera = malloc(sizeof(cabecera_t));
		cabecera->identificador = Kernel_ObtenerValorCompartida;
		send(kernelSocket, cabecera, sizeof(cabecera_t), 0);
		int *tamanio = malloc(sizeof(int));
		*tamanio = string_length(variable) + 1;
		send(kernelSocket, tamanio, sizeof(int), 0);
		send(kernelSocket, variable, *tamanio, 0);
		int valor;

		recv(kernelSocket, &valor, sizeof(valor), 0);

		free(cabecera);
		free(tamanio);
		return valor;
	}
	return 0;
}
t_valor_variable _asignarValorCompartida(t_nombre_compartida variable, t_valor_variable valor) {
	log_trace(logger, "Kernel asignarValorCompartida");
	if (finalizado == false) {
		cabecera_t *cabecera = malloc(sizeof(cabecera_t));
		cabecera->identificador = Kernel_AsignarValorCompartida;
		send(kernelSocket, cabecera, sizeof(cabecera_t), 0);
		int *tamanio = malloc(sizeof(int));
		*tamanio = string_length(variable) + 1;
		send(kernelSocket, tamanio, sizeof(int), 0);
		send(kernelSocket, variable, *tamanio, 0);
		send(kernelSocket, &valor, sizeof(t_valor_variable), 0);
		free(cabecera);
		free(tamanio);
		return valor;
	}
	return 0;
}

void _irAlLabel(t_nombre_etiqueta etiqueta) {

	log_trace(logger, "Ir a Label");
	if (finalizado == false) {
		if (pcbGlobal->tamanio_indice_etiquetas > 0) {
			void *buffer = malloc(pcbGlobal->tamanio_indice_etiquetas);
			if (finalizado == false) {
				if (solicitarDatos(umvSocket, pcbGlobal->indice_etiquetas, 0, pcbGlobal->tamanio_indice_etiquetas, buffer) == -1) {
					error_show("Segmentation Fault\n");
					log_error(logger, "Segmentation Fault");
					finalizarPrograma();
					return;
				}
			}
			pcbGlobal->programCounter = metadata_buscar_etiqueta(etiqueta, buffer, pcbGlobal->tamanio_indice_etiquetas);
			pcbGlobal->programCounter--;
			free(buffer);
		}
	}
}

void preservarContexto(t_nombre_etiqueta etiqueta, t_puntero *retorno) {
	if (finalizado == false) {
		int *cursorContexto = malloc(sizeof(t_puntero));
		*cursorContexto = pcbGlobal->cursorContexto;
		if (finalizado == false) {
			if (enviarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto + (pcbGlobal->tamanio_contexto_actual * 5), 4, cursorContexto) == -1) {
				error_show("Memory Overload\n");
				log_error(logger, "Memory Overload");
				finalizarPrograma();
				return;
			}
		}
		free(cursorContexto);
		int *pCounter = malloc(sizeof(t_puntero));
		*pCounter = pcbGlobal->programCounter + 1;
		if (finalizado == false) {
			if (enviarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto + (pcbGlobal->tamanio_contexto_actual * 5) + 4, 4, pCounter) == -1) {
				error_show("Memory Overload\n");
				log_error(logger, "Memory Overload");
				finalizarPrograma();
				return;
			}
		}
		free(pCounter);

		if (*retorno != 0) {
			int *direccionRetorno = malloc(sizeof(t_puntero));
			*direccionRetorno = *retorno;
			if (finalizado == false) {
				if (enviarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto + (pcbGlobal->tamanio_contexto_actual * 5) + 8, 4, direccionRetorno) == -1) {
					error_show("Memory Overload\n");
					log_error(logger, "Memory Overload");
					finalizarPrograma();
					return;
				}
			}

			pcbGlobal->cursorContexto += (pcbGlobal->tamanio_contexto_actual * 5) + 12;
			pcbGlobal->tamanio_contexto_actual = 0;
			free(direccionRetorno);
		} else {
			pcbGlobal->cursorContexto += (pcbGlobal->tamanio_contexto_actual * 5) + 8;
			pcbGlobal->tamanio_contexto_actual = 0;
		}
		list_clean(diccionarioVariables);
		log_trace(logger, "Contexto Preservado");
		_irAlLabel(etiqueta);
	}
}
void push(t_list *list, void *element) {
	list_add(list, element);

}
void _llamarSinRetorno(t_nombre_etiqueta etiqueta) {
	log_trace(logger, "Llamar sin Retorno");
	t_puntero sinRetorno = 0;

	preservarContexto(etiqueta, &sinRetorno);

}

void _llamarConRetorno(t_nombre_etiqueta etiqueta, t_puntero donde_retornar) {
	log_trace(logger, "Llamar con Retorno");
	preservarContexto(etiqueta, &donde_retornar);

}
void regenerar_contexto() {

	if (list_size(diccionarioVariables) != 0)
		list_clean(diccionarioVariables);
	int i;

	for (i = 0; i <= pcbGlobal->tamanio_contexto_actual * 5; i += 5) {
		variables_t *var = malloc(sizeof(variables_t));
		t_nombre_variable *variable = malloc(sizeof(t_nombre_variable));
		if (finalizado == false) {
			if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto + i, 1, variable) == -1) {
				error_show("Segmentation Fault\n");
				log_error(logger, "Segmentation Fault");
				finalizarPrograma();
				return;
			}
		}
		var->variable = *variable;
		var->offset = pcbGlobal->cursorContexto + i + 1;
		push(diccionarioVariables, var);
	}
	log_debug(logger, "Contexto Regenerado");
}
void _finalizar() {
	log_trace(logger, "Finalizar");
	if (pcbGlobal->cursorContexto != 0) {
		int *pCounter = malloc(sizeof(int));
		if (finalizado == false) {
			if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto - 4, 4, pCounter) == -1) {
				error_show("Segmentation Fault\n");
				log_error(logger, "Segmentation Fault");
				finalizarPrograma();
			}
		}
		pcbGlobal->programCounter = *pCounter;
		pcbGlobal->programCounter--;
		int *cursorAnterior = malloc(sizeof(int));
		if (finalizado == false) {
			if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto - 8, 4, cursorAnterior) == -1) {
				error_show("Segmentation Fault\n");
				log_error(logger, "Segmentation Fault");
				finalizarPrograma();
			}
		}
		pcbGlobal->tamanio_contexto_actual = (pcbGlobal->cursorContexto - 8 - *cursorAnterior) / 5;

		pcbGlobal->cursorContexto = *cursorAnterior;

		regenerar_contexto();
		free(pCounter);
		free(cursorAnterior);
	} else if (pcbGlobal->cursorContexto == 0) {
		finalizarPrograma();
	}
}

void _retornar(t_valor_variable retorno) {
	log_trace(logger, "Retornar");
	int *dondeRetornar = malloc(sizeof(int));
	if (finalizado == false) {
		if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto - 4, 4, dondeRetornar) == -1) {
			error_show("Segmentation Fault\n");
			log_error(logger, "Segmentation Fault");
			finalizarPrograma();
		}
	}
	int *pCounter = malloc(sizeof(int));
	if (finalizado == false) {
		if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto - 8, 4, pCounter) == -1) {
			error_show("Segmentation Fault\n");
			log_error(logger, "Segmentation Fault");
			finalizarPrograma();
		}
	}
	pcbGlobal->programCounter = *pCounter;
	pcbGlobal->programCounter--;
	int *cursorAnterior = malloc(sizeof(int));
	if (finalizado == false) {
		if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto - 12, 4, cursorAnterior) == -1) {
			error_show("Segmentation Fault\n");
			log_error(logger, "Segmentation Fault");
			finalizarPrograma();
		}
	}

	pcbGlobal->tamanio_contexto_actual = (pcbGlobal->cursorContexto - 12 - *cursorAnterior) / 5;

	pcbGlobal->cursorContexto = *cursorAnterior;

	regenerar_contexto();

	_asignar(*dondeRetornar, retorno);
}
void solicitudTonta() {
	void *buffer = malloc(10);
	if (solicitarDatos(umvSocket, pcbGlobal->stackBase, pcbGlobal->cursorContexto - 12, 4, buffer) == -1) {
		error_show("Segmentation Fault\n");
		log_error(logger, "Segmentation Fault");
		finalizarPrograma();
	}
	free(buffer);
}
void _imprimir(t_valor_variable valor_mostrar) {
	solicitudTonta();
	if (finalizado == false) {

		log_trace(logger, "IMPRIMIR");
		log_debug(logger, "El valor es %d", valor_mostrar);
		cabecera_t *cabecera = malloc(sizeof(cabecera_t));
		cabecera->identificador = Kernel_Imprimir;

		send(kernelSocket, (void *) cabecera, sizeof(cabecera_t), 0);
		send(kernelSocket, &pcbGlobal->pid, sizeof(uint32_t), 0);
		send(kernelSocket, &valor_mostrar, sizeof(t_valor_variable), 0);
		send(kernelSocket, &pcbGlobal->sock, 4, 0);
		free(cabecera);
	}

}

void _imprimirTexto(char* texto) {
	solicitudTonta();
	if (finalizado == false) {
		log_trace(logger, "IMPRIMIR TEXTO");
		log_debug(logger, "El valor es %s", (char *) texto);
		cabecera_t *cabecera = malloc(sizeof(cabecera_t));
		cabecera->identificador = Kernel_ImprimirTexto;
		send(kernelSocket, cabecera, sizeof(cabecera_t), 0);
		int tamanio = string_length(texto) + 1;
		send(kernelSocket, &pcbGlobal->pid, sizeof(uint32_t), 0);
		send(kernelSocket, &tamanio, sizeof(int), 0);
		send(kernelSocket, &pcbGlobal->sock, 4, 0);
		send(kernelSocket, texto, tamanio, 0);
		free(cabecera);
	}
}
void _entradaSalida(t_nombre_dispositivo dispositivo, int tiempo) {

	if (finalizado == false) {
		log_trace(logger, "ENTRADA SALIDA");
		log_debug(logger, "Nombre dispositivo:%s Tiempo: %d", dispositivo, tiempo);
		cabecera_t *cabecera = malloc(sizeof(cabecera_t));
		cabecera->identificador = Kernel_EntradaSalida;
		send(kernelSocket, (void *) cabecera, sizeof(cabecera_t), 0);
		pcbGlobal->programCounter++;
		send(kernelSocket, pcbGlobal, sizeof(pcb_t), 0);
		int tamanioNombre = string_length(dispositivo) + 1;
		send(kernelSocket, &tamanioNombre, sizeof(int), 0);
		send(kernelSocket, dispositivo, tamanioNombre, 0);
		send(kernelSocket, &tiempo, sizeof(tiempo), 0);

		finalizado = true;
		free(cabecera);
	}
}

void _wait(t_nombre_semaforo identificador_semaforo) {
	solicitudTonta();
	if(finalizado==false){
	log_trace(logger, "WAIT");
	log_debug(logger, "Semaforo: %s", identificador_semaforo);
	cabecera_t *cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = Kernel_Wait;
	send(kernelSocket, (void *) cabecera, sizeof(cabecera_t), 0);
	send(kernelSocket, &pcbGlobal->pid, sizeof(uint32_t), 0);
	int tamanioNombre = string_length(identificador_semaforo) + 1;
	send(kernelSocket, &tamanioNombre, sizeof(int), 0);
	send(kernelSocket, identificador_semaforo, tamanioNombre, 0);

	recv(kernelSocket, cabecera, sizeof(cabecera_t), 0);
	switch (cabecera->identificador) {
		case CPU_Interrumpir: {
			pcbGlobal->programCounter++;
			send(kernelSocket, pcbGlobal, sizeof(pcb_t), 0);
			finalizado = true;
			break;
		}
	}
	free(cabecera);}
}

void _signal(t_nombre_semaforo identificador_semaforo) {
	solicitudTonta();
	if(finalizado==false){
	log_trace(logger, "SIGNAL");
	log_debug(logger, "Semaforo: %s", identificador_semaforo);
	cabecera_t *cabecera = malloc(sizeof(cabecera_t));
	cabecera->identificador = Kernel_Signal;
	send(kernelSocket, (void *) cabecera, sizeof(cabecera_t), 0);

	int tamanioNombre = string_length(identificador_semaforo) + 1;
	send(kernelSocket, &tamanioNombre, sizeof(int), 0);
	send(kernelSocket, identificador_semaforo, tamanioNombre, 0);
	free(cabecera);
	}
}
