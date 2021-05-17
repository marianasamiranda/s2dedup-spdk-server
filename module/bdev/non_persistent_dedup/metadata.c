#include "metadata.h"
#include <stdlib.h>
#include "stdio.h"


uint64_t get(Metadata metadata, uint64_t laddr) {
    return metadata[laddr];
}

uint64_t put(Metadata metadata, uint64_t laddr, uint64_t paddr) {
    uint64_t existing_paddr = metadata[laddr];

    metadata[laddr] = paddr;

    return existing_paddr;
}

Metadata init_metadata(unsigned long size){
    Metadata  metadata = (Metadata) malloc(size * sizeof(Metadata));
    uint64_t init_i = 0;
    for (uint64_t i=0; i< size; i++){
    	metadata[i] = init_i;
    }
    return metadata;
}


void destroy_metadata(Metadata metadata){
    free(metadata);
}
