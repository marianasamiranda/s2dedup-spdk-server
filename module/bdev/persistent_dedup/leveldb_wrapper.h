
int create_leveldb_metadata(char* name);
int create_leveld_index(char* name);
int put_leveldb_metadata(const char* key, size_t keylen, const char* val, size_t vallen);
int put_leveldb_index(const char* key, size_t keylen, const char* val, size_t vallen);
void get_leveldb_metadata(const char* key, size_t keylen, char* dest);
void get_leveldb_index(const char* key, size_t keylen, char* dest);
int delete_leveldb_metadata(const char* key, size_t keylen);
int delete_leveldb_index(const char* key, size_t keylen);
void close_leveldb_metadata(void);
void close_leveldb_index(void);
int destroy_leveldb(char* name);
