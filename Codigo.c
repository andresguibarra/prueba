void compactacion() {
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
			j--;

			if ((&segmentosMemoria[i])->virtualExterna != -9999) {
				void *destino, *origen;

				origen = (int *) (&segmentosMemoria[i])->virtualExterna;
				destino = (int *) (&memAux[j])->virtualExterna;
				memcpy(destino, origen, cont);
				cont=1;
			} else {
				cont++;
			}
		}
	}
	segmentosMemoria = memAux;

	printf("hice la compactacion  %d %d \n", (&memAux[99])->idPCB, (&memAux[99])->idPCB);
	free(memAux);

}
