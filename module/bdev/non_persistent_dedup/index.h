#include <inttypes.h>
#include "glib.h"
#include "stdio.h"
#include "freeblocks.h"



typedef struct index_value
{
    uint64_t paddr;
    long nref;
} Index_value;


typedef struct indexas {
    GHashTable* hashA;
    GHashTable* hashB;
} Indexas;


typedef Indexas* Index;


uint64_t test_and_increment(Indexas* hashtable,  unsigned char *digest);
void test_and_decrement(Indexas* indexas,   uint64_t paddr);
void decrement(Indexas* hashtable, Index_value* index_node);
Indexas* init_index(unsigned long size);
void destroy_index(Indexas* indexas);
