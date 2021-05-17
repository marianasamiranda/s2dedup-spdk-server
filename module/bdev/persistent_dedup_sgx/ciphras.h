#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

int init_ciphras(int security_level, int epoch_or_threshold, int blocklen);
int destroy_ciphras(void);
int compute_hash(char* hash, char* src, uint64_t src_offset);
char* cipher_data_in(char* dest, uint64_t dest_offset, char* src, uint64_t src_offset);
char* cipher_data_out(char* dest, uint64_t dest_offset, char* src,  uint64_t src_offset);
