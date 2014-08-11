/*
 * serial.h
 *
 *  Created on: 11/05/2014
 *      Author: utnso
 */

#ifndef SERIAL_H_
#define SERIAL_H_

typedef struct Handshake {

	char tipo[8];

} idproceso_t;

typedef struct ProgramaActivo {
	int idPCB;
} activoprograma_t;

typedef struct CrearSegmento {
	int idPCB;
	int tamanio;
} crearsegmento_t;

typedef struct DestruirSegmento {
	int idPCB;
} destruirsegmento_t;
typedef struct Cabecera {
	int identificador;

} cabecera_t;

typedef struct Bytes {
	int base;
	int offset;
	int tamanio;
} bytes_t;

typedef enum Identificadores {
	IdHandshake,
	IdProgramaActivo,
	IdCrearSegmento,
	IdDestruirSegmento,
	IdSolicitarBytes,
	IdEnviarBytes,
	Kernel_ObtenerValorCompartida,
	Kernel_AsignarValorCompartida,
	Kernel_Imprimir,
	Kernel_ImprimirTexto,
	Kernel_EntradaSalida,
	Kernel_Wait,
	Kernel_Signal,
	CPU_PCB,
	CPU_Continuar,
	CPU_Apagado,
	CPU_Interrumpir,
	Quantum_cumplido,
	Id_ProgramaFinalizado,
	Kernel_Request,
	Programa_OK,
	Programa_Finalizar,
	Programa_ImprimirTexto,
	Programa_Imprimir
} Identificadores;
#endif /* SERIAL_H_ */
