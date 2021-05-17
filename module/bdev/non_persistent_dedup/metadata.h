#include <inttypes.h>
#include <glib.h>
#include "index.h"


typedef uint64_t* Metadata;
uint64_t get(Metadata metadata, uint64_t laddr) ;
uint64_t put(Metadata metadata, uint64_t laddr, uint64_t paddr);
Metadata init_metadata(unsigned long size);
void destroy_metatada(Metadata metadata);
