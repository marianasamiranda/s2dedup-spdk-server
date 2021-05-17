#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include "metadata.h"
#include <stdbool.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include "spdk/bdev_module.h"
#include <semaphore.h>
#include "spdk/queue.h"
#include "spdk/event.h"


#define HASH_LEN 32

sem_t mutex;

int BLOCKLEN;

long int deduplicated_blocks;
long int unique_blocks;

typedef struct dedupas{
    Metadata metadata;
    Index index;
} Dedupas;


static Dedupas dedup;

void init_dedup(unsigned long data_size, unsigned long metadata_size, unsigned long blocklen);
void init_dedup_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, int data_size, int metadata_size, int blocklen);

uint64_t read_block(uint64_t offset) ;
uint64_t write_block(uint64_t offset, char * content);
uint64_t flush(uint64_t offset);
uint64_t unmap(uint64_t offset);

uint64_t reset_dedup(void);

struct spdk_poller* poller;

FILE * fp;

uint64_t reset_dedup(void) {
	close_freeblocks();
	spdk_poller_unregister(&poller);
	fclose(fp);
	destroy_metadata(dedup.metadata);
	destroy_index(dedup.index);
	return 0;
}

uint64_t read_block(uint64_t offset) {
	sem_wait(&mutex);
	uint64_t result = get(dedup.metadata,offset);
	sem_post(&mutex);

	return result;
}

uint64_t flush(uint64_t offset) {
    return 0;
}

uint64_t unmap(uint64_t offset) {
    return 0;
}

uint64_t write_block(uint64_t offset, char * content) {
	unsigned char* digest;
	unsigned  char *key = (unsigned char*) malloc (sizeof(unsigned char) * HASH_LEN);
	if (content ==NULL){
		printf("Content is null\n");
		sleep(5);
		digest = SHA256("",0,0);
		memcpy(key,digest, HASH_LEN);
	} else {
		compute_hash(key, content, offset * BLOCKLEN);
	}
	
	uint64_t mask = 0x8000000000000000; 
	
	uint64_t paddr = test_and_increment(dedup.index, key);
	uint64_t  new = paddr & mask;
	
	if (new) {
		paddr = paddr & ~mask;
		unique_blocks ++;
	} else {
		deduplicated_blocks++;
	}
	
	uint64_t result = put(dedup.metadata ,offset, paddr);
	
	test_and_decrement(dedup.index, result);
	
	if (new) return paddr;
	return -1;
}

void stat_printer(){
	fprintf(fp, "Deduplicated blocks: %ld, Unique blocks: %ld \n", deduplicated_blocks, unique_blocks);
	fflush(fp);
}

void init_dedup(unsigned long data_size, unsigned long metadata_size, unsigned long blocklen){
	
	fp = fopen("module/bdev/non_persistent_dedup_sgx/dedup_ratio.res","w+");
	
	sem_init(&mutex, 0, 1);
	sem_wait(&mutex);
	
	BLOCKLEN = blocklen;
	
	dedup.metadata = init_metadata(data_size);
	dedup.index = init_index(data_size);
	
	deduplicated_blocks =0;
	unique_blocks = 0;
	
	poller = SPDK_POLLER_REGISTER(stat_printer, NULL, 30000000);
	
	sem_post(&mutex);
}

void init_dedup_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, int data_size, int metadata_size, int blocklen ){
	init_freeblocks(desc, ch, data_size, metadata_size, blocklen);
}
