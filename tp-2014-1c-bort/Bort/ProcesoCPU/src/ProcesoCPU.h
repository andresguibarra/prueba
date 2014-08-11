/*
 * ProcesoCPU.h
 *
 *  Created on: 01/06/2014
 *      Author: utnso
 */

#ifndef PROCESOCPU_H_
#define PROCESOCPU_H_

#include <commons/collections/list.h>

typedef struct ProcessControlBlock {
	uint32_t pid;
	t_puntero codigo_segmento;
	t_puntero stackBase;
	uint32_t cursorContexto;

	t_puntero indice_etiquetas;
	t_puntero_instruccion programCounter;
	uint32_t tamanio_contexto_actual;
	uint32_t tamanio_indice_etiquetas;
	t_puntero_instruccion indice_codigo;
	int sock;
} pcb_t;
//__attribute__ ((__packed__))

typedef struct Variables {
	t_puntero offset;
	t_nombre_variable variable;
} variables_t;

typedef struct Contexto {
	t_list *contexto;
	t_nombre_etiqueta nombre;
	t_puntero retorno;
	t_valor_variable programCounter;
} context_t;

t_puntero _definirVariable(t_nombre_variable variable);
t_puntero _obtenerPosicionVariable(t_nombre_variable identificador_variable);
t_valor_variable _dereferenciar(t_puntero direccion_variable);
void _asignar(t_puntero direccion_variable, t_valor_variable valor);

t_valor_variable _obtenerValorCompartida(t_nombre_compartida variable);
t_valor_variable _asignarValorCompartida(t_nombre_compartida variable, t_valor_variable valor);
void  _irAlLabel(t_nombre_etiqueta etiqueta);
void _llamarSinRetorno(t_nombre_etiqueta etiqueta);
void _llamarConRetorno(t_nombre_etiqueta etiqueta, t_puntero donde_retornar);
void _finalizar();
void _retornar(t_valor_variable retorno);
void _imprimir(t_valor_variable valor_mostrar);
void _imprimirTexto(char* texto);
void _entradaSalida(t_nombre_dispositivo dispositivo, int tiempo);

void _wait(t_nombre_semaforo identificador_semaforo);
void _signal(t_nombre_semaforo identificador_semaforo);

#endif /* PROCESOCPU_H_ */
