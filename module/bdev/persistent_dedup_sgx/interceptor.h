#include <inttypes.h>
#include "metadata.h"
#include <inttypes.h>
//#include "leveldb_wrapper.h"

uint64_t reset_dedup();
uint64_t read_block(uint64_t offset);
uint64_t write_block( uint64_t offset, char *content);
uint64_t flush(uint64_t offset);
uint64_t unmap(uint64_t offset);
void init_dedup(unsigned long data_size, unsigned long metadata_size, unsigned long blocklen);
void init_dedup_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, int data_size, int metadata_size, int blocklen);
