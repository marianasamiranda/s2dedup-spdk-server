#include "index.h"


static gboolean uint64_t_equal (gconstpointer v1, gconstpointer v2){
	return ((const uint64_t) v1) == ((const uint64_t) v2);
}

static guint uint64_t_hash(gconstpointer v){
	return (guint) (const uint64_t) v;
}

static gboolean uchar_equal(gconstpointer v1, gconstpointer v2){
	unsigned char * h1 = (unsigned char*) v1;
	unsigned char * h2 = (unsigned char*) v2;

	int rest = memcmp((unsigned char*) v1, (unsigned char *) v2,32);

	return rest==0;
}

static guint uchar_hash(gconstpointer v){
  	const signed char *p;
  	guint32 h = 5381;

	p=v;
  	for (int i=0; i<32; i++){
    		h = (h << 5) + h + *p;
		p++;
	}

	return h;
}

uint64_t test_and_increment(Indexas* indexas, unsigned char *digest){
	char *old_key;
	
	Index_value *value;
	
	uint64_t mask = 0x8000000000000000;
	
	if (g_hash_table_lookup_extended(indexas->hashA, digest, &old_key, &value)) {
		value -> nref ++;
		mask=0x0;
		g_hash_table_replace (indexas->hashA, old_key, value);
		free(digest);
	} else {
		uint64_t free_block = get_freeblock();
		value = (Index_value*)malloc(sizeof(struct index_value));
		value -> paddr = free_block;
		value -> nref = 1;
		g_hash_table_insert (indexas->hashA, digest, value);
	       	g_hash_table_insert (indexas->hashB, free_block, digest);
	}
	
	uint64_t res = (value ->paddr) | mask;
	return res;
}

void test_and_decrement(Indexas* indexas, uint64_t paddr){
	unsigned char* hash = g_hash_table_lookup(indexas->hashB, paddr);
	Index_value* value = g_hash_table_lookup(indexas->hashA, hash);
	
	if (value) {
		value -> nref --;
		
		if (value -> nref == 0) {
			put_freeblock(value -> paddr);
			free(value);
			g_hash_table_remove(indexas->hashA, hash);
			g_hash_table_remove(indexas->hashB, paddr);
			free(hash);
		} else {
			g_hash_table_replace (indexas->hashA, hash, value);
		}
	} else {
	    printf("ERROR IN test_and_decrement!\n");
	}
}

/*!
 * \brief Free a key-value pair inside the hash table.
 */
static void free_a_hash_table_entry (gpointer key, gpointer value, gpointer user_data){
	g_free (key);
	g_free (value);
}

/*!
 * \brief Free all key-value entries in the hash table.
 */
int free_all_key_value_entries (GHashTable* table){
	g_hash_table_foreach (table, free_a_hash_table_entry, NULL);
}

void destroy_index(Indexas* indexas){
	free_all_key_value_entries(indexas->hashA);
	g_hash_table_destroy(indexas->hashB);
	g_hash_table_destroy(indexas->hashA);
	free(indexas);
}

Indexas* init_index(unsigned long size){
	Indexas* indexas = (Indexas*) malloc(sizeof(Indexas*));
	indexas->hashA = g_hash_table_new(uchar_hash, uchar_equal);
	indexas->hashB = g_hash_table_new(uint64_t_hash, uint64_t_equal);
	
	uint64_t free_block = 0;
	Index_value*  value = (Index_value*)malloc(sizeof(struct index_value));
	value -> paddr = free_block;
	value -> nref = size;
	unsigned char * digest = (unsigned char *) malloc (sizeof(unsigned char *) * 32);
	memset(digest, '0', 32);
	
	g_hash_table_insert (indexas->hashA, digest, value);
	g_hash_table_insert (indexas->hashB, free_block, digest);

	return indexas;
}

