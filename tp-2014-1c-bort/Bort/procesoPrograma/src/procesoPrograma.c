/*
 ============================================================================
 Name        : procesoPrograma.c
 Author      : 
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

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
#include <commons/log.h>

#define PACKAGESIZE 1024

char *IP; //IP Kernel
char *PUERTO;  //PUERTO Kernel
char *archivoConfig;  //referencia al archivo de configuracion
char message[PACKAGESIZE];
int kernelSocket;
int enviarPorSockets();
int kernelRequest();
char* temp_file = "logPP"; //archivo done van los logs
t_log* logger;

int main(int argc, char **argv) {
	//creo el archivo de logs del tipo TRACE
	logger = log_create(temp_file, "logPP", false, LOG_LEVEL_TRACE);
	log_trace(logger, "Se inicia el Proceso Programa");
	//leo IP y PUERTO del Kernel desde mi archivo de configuracion
	archivoConfig = malloc(sizeof("/archivo.config"));
	archivoConfig = "archivo.config";
	t_config *miConfig = config_create(archivoConfig);
	IP = malloc(16);
	IP = string_duplicate("");

	if (config_has_property(miConfig, "IP")) {
		IP = string_duplicate(config_get_string_value(miConfig, "IP"));
	} else if (string_length(IP) == 0) {
		log_error(logger,
				"No se pudo leer la direccion IP del Kernel. PROCESS ABORTED");
		return 0;
	}

	PUERTO = malloc(6);
	PUERTO = string_duplicate("");
	if (config_has_property(miConfig, "PUERTO")) {
		PUERTO = string_duplicate(config_get_string_value(miConfig, "PUERTO"));
	} else if (string_length(PUERTO) == 0) {
		log_error(logger,
				"No se pudo leer el puerto de conexion con el Kernel. PROCESS ABORTED");
		return 0;
	}

	//le paso el codigo ansisop como primer argumento
	enviarPorSockets(argv);

	config_destroy(miConfig);
	return 0;
}

int enviarPorSockets(char **codigo) {
	struct addrinfo hints;
	struct addrinfo *serverInfo;
	char *Buffer;
	int conex;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	getaddrinfo(IP, PUERTO, &hints, &serverInfo);

	kernelSocket = socket(serverInfo->ai_family, serverInfo->ai_socktype,
			serverInfo->ai_protocol);

	conex = connect(kernelSocket, serverInfo->ai_addr, serverInfo->ai_addrlen);

	if (conex == -1) {
		log_error(logger,
				"No se ha podido conectar con el Kernel. PROCESS ABORTED");
		return 0;
	}

	log_trace(logger, "Conexion exitosa con el Kernel. Puerto de conexion: %s",
			PUERTO);
	freeaddrinfo(serverInfo);

	//Pregunto al Kernel si esta listo para recibir el codigo
	log_trace(logger, "Se pide permiso para enviar codigo al Kernel");
	kernelRequest();

	//Se procede a enviar el codigo ansisop
	memcpy(message, codigo, PACKAGESIZE);  //abro el archivo .ansisop
	FILE *archivo;
	if ((archivo = fopen(codigo[1], "rb")) == 0) {
		log_error(logger,
				"No se pudo leer el archivo con el codigo ansisop. PROCESS ABORTED");
		return 0;  //se corta el proceso si el archivo esta vacio
	}

	log_trace(logger, "Se envia el codigo al Kernel"); //comienza el envio del codigo

	fseek(archivo, 0L, SEEK_END); //se saca el tamaño del codigo

	int tamanio = ftell(archivo); //creo variable tamanio con el tamaño del codigo
	send(kernelSocket, &tamanio, sizeof(int), 0); //envio tamanio para que el Kernel haga malloc con el mismo
	//pone el stream stream al principio del archivo
	rewind(archivo);
	//empiezo a leer el codigo y lo almaceno en el buffer para enviarlo
	Buffer = malloc(tamanio);
	fread(Buffer, tamanio, 1, archivo);
	send(kernelSocket, (void *) Buffer, tamanio, 0); //envia el codigo

	log_trace(logger, "El codigo se envio exitosamente. Tamaño = %ld bytes",
			tamanio);

	//quedo a la espera para imprimir lo que mande el kernel
	log_info(logger, "Se envio el codigo, a la espera de impresiones");
	log_warning(logger,
			"Si luego de las impresiones no se recibe la señal el procesoPrograma continuara ejecutandose");

	bool escuchar = true;
	while (escuchar) {
		cabecera_t cabecera;
		recv(kernelSocket, &cabecera, sizeof(cabecera_t), 0);

		switch (cabecera.identificador) {
		case Programa_ImprimirTexto: {

			int *sizeOfText = malloc(sizeof(int));
			recv(kernelSocket, sizeOfText, sizeof(int), 0);
			void *bufferText = malloc(*sizeOfText);
			recv(kernelSocket, bufferText, *sizeOfText, 0);

			printf("%s\n", (char*) bufferText);
			log_info(logger, "El Kernel mando imprimir '%s'", bufferText);

			free(bufferText);
			free(sizeOfText);

			break;
		}
		case Programa_Imprimir: {
			int var;
			recv(kernelSocket, &var, sizeof(int), 0);
			printf("%d\n", var);
			log_info(logger, "El Kernel mando imprimir '%d'", var);

			break;
		}
		case Programa_Finalizar: {
			escuchar = false;

			break;
		}
		}
	}
	log_info(logger, "Se cierra el socket. Fin de comunicacion con el Kernel");
	close(kernelSocket);
	log_info(logger, "PROCESS SUCCESSFULLY ENDED");
	return 0;
}

int kernelRequest() {
	cabecera_t *cabeceraPedido = malloc(sizeof(cabecera_t));
	cabeceraPedido->identificador = Kernel_Request;
	send(kernelSocket, (void*) cabeceraPedido, sizeof(cabecera_t), 0); //Se envia el pedido

	cabecera_t cabeceraRespuesta;
	recv(kernelSocket, &cabeceraRespuesta, sizeof(cabecera_t), 0); //Se recibe la respuesta

	switch (cabeceraRespuesta.identificador) { //Se evalua la respuesta
	case Programa_OK: {
		log_info(logger, "Se obtiene respuesta OK del Kernel");

		break;
	}
	default:{
		log_error(logger,
				"FATAL ERROR: No se recibio respuesta valida del Kernel. PROCESS ABORTED");
		exit(EXIT_FAILURE);

		break;
	}
	}
	free(cabeceraPedido);
	return 0;
}
