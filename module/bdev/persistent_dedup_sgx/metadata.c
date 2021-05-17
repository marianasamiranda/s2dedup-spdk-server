#include "metadata.h"
#include <stdlib.h>
#include "stdio.h"


uint64_t get(uint64_t laddr) {
    uint64_t result;


    get_leveldb_metadata((char*) &laddr, sizeof(uint64_t), (char*) &result);
	
    return result;
}

uint64_t put(uint64_t laddr, uint64_t paddr) {
    uint64_t existing_paddr;

    get_leveldb_metadata((char*) &laddr, sizeof(uint64_t), (char*) &existing_paddr);
   
    put_leveldb_metadata((char*) &laddr, sizeof(uint64_t), (char*) &paddr , sizeof(uint64_t));

    return existing_paddr;
}

void init_metadata(unsigned long size){
    
    uint64_t init_i = 0;
        
    for(uint64_t i=0; i<size; i++){
	    put_leveldb_metadata((char*) &i, sizeof(uint64_t), (char*) &init_i, sizeof(uint64_t));
    }
}
