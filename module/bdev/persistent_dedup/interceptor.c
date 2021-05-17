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
int DATA_SIZE;
int METADATA_SIZE;
long int deduplicated_blocks;
long int unique_blocks;

void init_dedup(unsigned long data_size, unsigned long metadata_size, unsigned long blocklen);
void init_dedup_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, int data_size, int metadata_size, int blocklen);

uint64_t read_block(uint64_t offset);
uint64_t write_block(uint64_t offset, char * content);
uint64_t flush(uint64_t offset);
uint64_t unmap(uint64_t offset);

uint64_t reset_dedup(void);

FILE *fp;

struct spdk_poller* poller;

uint64_t reset_dedup() {
	spdk_poller_unregister(&poller);
  	close_freeblocks(); 
	fclose(fp);
	return 0;
}

uint64_t read_block(uint64_t offset) {
	sem_wait(&mutex);
	uint64_t result = get(offset);
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
    	if (content ==NULL){ 
	    	printf("Content is null\n");
		sleep(5);
		digest = SHA256("",0,0); 
	}
	else {
	
	        digest = SHA256(content, BLOCKLEN, 0);
	}
	
	uint64_t mask = 0x8000000000000000;
	uint64_t paddr = test_and_increment(digest);
	uint64_t  new = paddr & mask;

	if (new) {
		paddr = paddr & ~mask;
        	unique_blocks ++;	
    	}
    	else{
		deduplicated_blocks++;
    	}
	
	uint64_t result = put(offset, paddr);
	
	test_and_decrement(result);

	if (new) return paddr;
    	return -1;
}

void deal_db(){
	printf("Detroying DBs\n");
	destroy_leveldb("module/bdev/persistent_dedup/dbs/metadata");
	destroy_leveldb("module/bdev/persistent_dedup/dbs/index");
	printf("DBs Destroyed\n");
	printf("Creating DBs\n");
	create_leveldb_metadata("module/bdev/persistent_dedup/dbs/metadata");
	create_leveldb_index("module/bdev/persistent_dedup/dbs/index");
	printf("DBs created\n");
	sem_post(&mutex);
}

void stat_printer(){
	fprintf(fp, "Deduplicated blocks: %ld, Unique blocks: %ld\n", deduplicated_blocks, unique_blocks);
	fflush(fp);
}

void init_dedup(unsigned long data_size, unsigned long metadata_size, unsigned long blocklen){
	fp = fopen("module/bdev/persistent_dedup/dedup_ratio.res","w+");
	
	sem_init(&mutex, 0, 1);
	sem_wait(&mutex);
	deal_db();
	
	BLOCKLEN = blocklen;
	METADATA_SIZE = metadata_size;
	DATA_SIZE =  data_size;
	
	init_metadata(DATA_SIZE);
	
	deduplicated_blocks =0;
	unique_blocks = 0;
	
	poller = SPDK_POLLER_REGISTER(stat_printer, NULL, 30000000);
	
	init_index(DATA_SIZE);
}

void init_dedup_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, int data_size, int metadata_size, int blocklen ){
	init_freeblocks(desc, ch, data_size, metadata_size, blocklen);
}
