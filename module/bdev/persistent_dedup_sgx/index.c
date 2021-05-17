#include "index.h"

#define HASH_LEN 32


uint64_t test_and_increment(unsigned char *digest){
	Index_value* value=NULL;
	get_leveldb_index(digest, HASH_LEN, (char*) &value);
	
	uint64_t mask = 0x8000000000000000;
	
	if (value) {
		value -> nref ++;
		mask=0x0;
		put_leveldb_index( digest, HASH_LEN, (char*) &value, sizeof(struct index_value));
	} else {
		uint64_t free_block = get_freeblock();
		
		value = (Index_value*) malloc(sizeof(struct index_value));
		value -> paddr = free_block;
		value -> nref = 1;
		
		put_leveldb_index(digest, HASH_LEN, (char*) &value, sizeof(struct index_value));
		put_leveldb_index((char*) &free_block, sizeof(uint64_t), digest, HASH_LEN);
	}
	
	uint64_t res = (value ->paddr) | mask;
	return res;
}

void test_and_decrement(uint64_t paddr){
	unsigned char hash[HASH_LEN];
	unsigned char hash2[HASH_LEN];
	get_leveldb_index((char*) &paddr, sizeof(uint64_t), hash);
	
	memcpy(hash2, hash, HASH_LEN);
	
	Index_value*  value = NULL;
	get_leveldb_index( hash, HASH_LEN, (char*) &value);
	
	if (value) {
		value -> nref --;
		if (value -> nref == 0) {
			put_freeblock(value -> paddr);
			delete_leveldb_index( hash2, HASH_LEN);
			delete_leveldb_index((char*) &paddr, sizeof(uint64_t));
			free(value);
		} else {
			put_leveldb_index( hash2, HASH_LEN, (char*) &value, sizeof(struct index_value));
		}
	} else {
		printf("ERROR in test_and_decrement\n");
	}
}

void init_index(unsigned long size){
	uint64_t free_block = 0;
	Index_value*  value = (Index_value*)malloc(sizeof(struct index_value));
	value -> paddr = free_block;
	value -> nref = size;
	
	unsigned char * digest = (unsigned char *) malloc (sizeof(unsigned char *) * 32);
	memset(digest, '0', 32);
	
	put_leveldb_index( digest, HASH_LEN, (char*) &value, sizeof(struct index_value));
	put_leveldb_index((char*) &free_block, sizeof(uint64_t), digest, HASH_LEN);
	
	free(digest);
}

