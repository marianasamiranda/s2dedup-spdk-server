#include "freeblocks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "spdk/env.h"
#include <sys/syscall.h>


uint64_t read_ptr_addr;
uint64_t write_ptr_addr;
uint64_t init_ptr_addr;

uint64_t read_block_counter;
uint64_t write_block_counter;

uint32_t total_freeblocks_blocks;

uint64_t * write_cache;
uint64_t * read_cache;

int write_cache_counter;
int read_cache_counter;
int cache_max;
int read_max;

unsigned long blocklen;

sem_t mutex_freeblock;

char * buff;

void ** buff_addr;

struct spdk_io_channel * ch2;

struct spdk_bdev_desc *desc2;


static void _freeblocks_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
        /* Complete the original IO and then free the one that we created here
         * as a result of issuing an IO via submit_request.
         */
        spdk_bdev_free_io(bdev_io);
}

static void _freeblocks_complete_read_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	sem_post(&mutex_freeblock);
        
	/* Complete the original IO and then free the one that we created here
         * as a result of issuing an IO via submit_request.
         */
        spdk_bdev_free_io(bdev_io);
}


uint64_t get_freeblock(){


 	if (read_cache_counter == 0){
		sem_wait(&mutex_freeblock);
		sem_post(&mutex_freeblock);
	}

 	if (read_block_counter == write_block_counter && read_cache_counter == read_max) {
		if (read_max == cache_max){ 
			read_cache_counter = 0;
		}
		read_max = write_cache_counter;
		int temp_counter = 0;
		
		while(temp_counter<read_max){
			read_cache[temp_counter] = write_cache[temp_counter];
			temp_counter++;
		}	
	}
	
	uint64_t new_paddr = read_cache[read_cache_counter];
	
	read_cache_counter++;
	
	if (read_cache_counter == read_max) {
		
		sem_wait(&mutex_freeblock);
		
		int rc = spdk_bdev_read(desc2, ch2, read_cache, init_ptr_addr + read_block_counter * blocklen*8*2, blocklen*8*2, _freeblocks_complete_read_io, NULL);
		
		if (read_block_counter == 0) {
			spdk_free(buff_addr[total_freeblocks_blocks - 1]);
		} else {
			spdk_free(buff_addr[read_block_counter - 1]);
		}
		
		if (rc != 0){
			printf("ERROR IN get_freeblock\n");
		}
			
		if(read_max == cache_max){
			read_cache_counter = 0;
		}
		
		read_block_counter++;
		read_max = cache_max;
		
		if (read_block_counter == total_freeblocks_blocks){
			read_block_counter = 0;
		}
		
		snprintf(buff, blocklen, "%" PRIu64 , read_block_counter);
		
		rc = spdk_bdev_write(desc2, ch2, buff, read_ptr_addr, blocklen, _freeblocks_complete_io, NULL);
		if (rc != 0){
			printf("ERROR IN pointer update\n");
		}
	}
	return new_paddr;
}

void put_freeblock(uint64_t paddr){
	
	write_cache[write_cache_counter] = paddr;
	write_cache_counter++;
	
	if (write_cache_counter == cache_max) {
		int rc = spdk_bdev_write(desc2, ch2, write_cache, init_ptr_addr + write_block_counter * blocklen *8*2 , blocklen*8*2, _freeblocks_complete_io, NULL);
		
		buff_addr[write_block_counter] = write_cache;
		
		write_cache = spdk_malloc(blocklen*8*2, 0, NULL,SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
		
		if (rc != 0){
			printf("ERROR IN put_freeblock\n");
		}
		
		write_cache_counter = 0;
		write_block_counter++;
		
		if (write_block_counter == total_freeblocks_blocks){
			write_block_counter = 0;
		}
		
		snprintf(buff, blocklen, "%" PRIu64 , write_block_counter);
		
		rc = spdk_bdev_write(desc2, ch2, buff, write_ptr_addr, blocklen, _freeblocks_complete_io, NULL);
		if (rc != 0){
			printf("ERROR IN pointer update\n");
		}
	}
}

void init_freeblocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch, unsigned long data_size, unsigned long metadata_size, unsigned long _blocklen){
	
	blocklen = _blocklen;
	total_freeblocks_blocks = (metadata_size - 2) / 16;
	
	write_cache_counter = 0;
	cache_max = blocklen / 8 * 8 * 2;
	read_max = cache_max;
	read_cache_counter = 0;
	read_block_counter = 0;
	write_block_counter = 0;
	
	init_ptr_addr =  (data_size + 2) * blocklen ;
	read_ptr_addr = init_ptr_addr - 2 * blocklen;
	write_ptr_addr = init_ptr_addr - 1 * blocklen;
	
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(desc);
	uint32_t blk_size, buf_align;
	
	blk_size = spdk_bdev_get_block_size(bdev);
	buf_align = spdk_bdev_get_buf_align(bdev);
	
	ch2 = ch;
	desc2 = desc;
	
	buff = spdk_dma_malloc(512, buf_align, NULL);
	
	write_cache = spdk_malloc(blocklen*8*2, 0, NULL,SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	
	buff_addr = malloc( total_freeblocks_blocks * sizeof(void *));
	
	if (!buff) {
		printf("Failed to allocate buffer\n");
	}
	
	snprintf(buff, 512, "%" PRIu64 , write_block_counter);
	
	int rc = spdk_bdev_write(desc2, ch2, buff, read_ptr_addr, blk_size, _freeblocks_complete_io, NULL);
	if (rc != 0){
		printf("ERROR IN init_freeblocks\n");
	}
	
	rc = spdk_bdev_write(desc2, ch2, buff, write_ptr_addr, blk_size, _freeblocks_complete_io, NULL);
	if (rc != 0){
		printf("ERROR IN init_freeblocks\n");
	}
	
	for(uint64_t i=1; i< data_size; i++){
		put_freeblock(i);
	}
	
	read_cache = spdk_dma_malloc(blocklen*8*2, 0, NULL);
	sem_init(&mutex_freeblock, 0, 1);
	
	rc = spdk_bdev_read(desc2, ch, read_cache, init_ptr_addr + read_block_counter * blocklen*8*2, blocklen*8*2, _freeblocks_complete_read_io, NULL);
	
	sem_wait(&mutex_freeblock);
	
	read_block_counter++;
	
	if (rc != 0){
		printf("ERROR IN init_freeblocks\n");
	}
}

void close_freeblocks(){
	struct spdk_thread * ch_thread = spdk_io_channel_get_thread(ch2);
	spdk_thread_send_msg(ch_thread, spdk_put_io_channel, ch2);
}
