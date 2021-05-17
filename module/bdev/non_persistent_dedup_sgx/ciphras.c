#include "ciphras.h"
#include "sgx_urts.h"

#define KEY_SIZE 64 
#define TWEAK_SIZE  16
#define HASH_LEN 32


unsigned char CLIENT_KEY[KEY_SIZE]  = "C53C0E2F1B0B19AC53C0E2F1B0B19AAC53C0E2F1B0B19AC53C0E2F1B0B19AAA";

int isecurity_level;
int iepoch_or_threshold;
int iblocklen;

sgx_enclave_id_t global_eid = 0;


int _sgxCreateEnclave(void);
int _sgxDestroyEnclave(void);

/* SGX Function for enclave creation */
int _sgxCreateEnclave(void) {
    char *enclavefilepath = (char*) "module/bdev/non_persistent_dedup_sgx/Enclave.signed.so";
        sgx_launch_token_t token = {0};
        int updated = 0;
        sgx_status_t ret;
        ret = sgx_create_enclave(enclavefilepath, SGX_DEBUG_FLAG, &token, &updated, &global_eid, NULL );
    if (SGX_SUCCESS != ret) printf("sgxCreateEnclave: cant create Enclave (error 0x%x)\n", ret );
    return ret;
}

/* destroy SGX enclave */
int _sgxDestroyEnclave(void) {
        sgx_status_t ret;
    if ((ret = trusted_clear_sgx(global_eid)) != SGX_SUCCESS) printf("trustedClear: error 0x%x\n", ret);
        if ((ret = sgx_destroy_enclave(global_eid)) != SGX_SUCCESS) printf("sgxDestroyEnclave: cant destroy Enclave (error 0x%x)\n", ret );
        else printf("ENCLAVE DESTROYED\n");
        return ret;
}

void _recreateEnclave() {

    /* if the enclave is lost, release its resources, and bring the enclave back up. */
    if (SGX_SUCCESS != _sgxDestroyEnclave()) exit(EXIT_FAILURE);
    if (SGX_SUCCESS != _sgxCreateEnclave()) exit(EXIT_FAILURE);        
    if (SGX_SUCCESS != trusted_init_sgx(global_eid, CLIENT_KEY, KEY_SIZE, TWEAK_SIZE, isecurity_level, iepoch_or_threshold))  exit(EXIT_FAILURE);
    
    printf("[ENCLAVE_LOST] New enclave id = %d\n", (int) global_eid);    
}


char* cipher_data_in(char* dest, uint64_t dest_offset, char* src, uint64_t src_offset) {
	unsigned char *enclaveciphertext = (unsigned char*) malloc(sizeof(unsigned char) * iblocklen);
	
	int res=0;
        int ciphertext_size = iblocklen;

        while (trusted_reencrypt(global_eid, &res, enclaveciphertext, ciphertext_size, dest_offset, src, ciphertext_size, src_offset) == SGX_ERROR_ENCLAVE_LOST)    
		_recreateEnclave();

	memcpy(dest, enclaveciphertext, iblocklen);
	free(enclaveciphertext);	
	return enclaveciphertext;
}

char* cipher_data_out(char* dest, uint64_t dest_offset, char* src, uint64_t src_offset) {
        unsigned char *enclaveciphertext = (unsigned char*) malloc(sizeof(unsigned char) * iblocklen);

        int res=0;
        int ciphertext_size = iblocklen;

        while (trusted_reencrypt_reverse(global_eid, &res, enclaveciphertext, ciphertext_size, dest_offset, src, ciphertext_size, src_offset) == SGX_ERROR_ENCLAVE_LOST)
		_recreateEnclave();

        memcpy(dest, enclaveciphertext, iblocklen);
        free(enclaveciphertext);
        return enclaveciphertext;
}



int compute_hash(char* hash, char* src, uint64_t src_offset) {
	int res = 0;
	while (trusted_compute_hash(global_eid, &res, hash, HASH_LEN, src, iblocklen, src_offset) == SGX_ERROR_ENCLAVE_LOST)
                _recreateEnclave();
}

int init_ciphras(int security_level, int epoch_or_threshold, int blocklen) {
	
	isecurity_level = security_level;
	iepoch_or_threshold = epoch_or_threshold;
	iblocklen = blocklen;

	sgx_status_t err = _sgxCreateEnclave();
	if (err != SGX_SUCCESS) print_sgx_error_message(err);
	
	err = trusted_init_sgx(global_eid, CLIENT_KEY, KEY_SIZE, TWEAK_SIZE, isecurity_level, iepoch_or_threshold);
	if (err != SGX_SUCCESS) print_sgx_error_message(err);

	auth_init(KEY_SIZE, TWEAK_SIZE);

	return err;
}


int destroy_ciphras(void) {
	sgx_status_t err = _sgxDestroyEnclave();

	return err;
}
