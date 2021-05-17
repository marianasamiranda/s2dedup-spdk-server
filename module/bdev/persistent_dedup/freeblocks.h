#include <inttypes.h>
#include "vbdev_persistent_dedupas.h"

uint64_t get_freeblock();
void put_freeblock(uint64_t paddr);
void init_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, unsigned long data_size, unsigned long metadata_size, unsigned long blocklen);
void close_freeblocks();

