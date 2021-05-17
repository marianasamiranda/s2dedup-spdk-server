#include <inttypes.h>
#include "index.h"


uint64_t get(uint64_t laddr) ;
uint64_t put(uint64_t laddr, uint64_t paddr);
void init_metadata(unsigned long size);
