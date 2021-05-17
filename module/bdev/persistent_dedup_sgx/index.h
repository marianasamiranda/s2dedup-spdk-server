#include <inttypes.h>
#include "stdio.h"
#include "freeblocks.h"
#include "spdk/bdev_module.h"


typedef struct index_value
{
    uint64_t paddr;
    long nref;
} Index_value;


uint64_t test_and_increment(unsigned char *digest);
void test_and_decrement( uint64_t paddr);
void init_index(unsigned long size);
void destroy_index(void);
