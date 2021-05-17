#include <leveldb/c.h>
#include <stdio.h>
#include "leveldb_wrapper.h"
#include <string.h>
#include <stdlib.h>

leveldb_t *db_index;
leveldb_t *db_metadata;


/**********************************METADATA*********************************/

int create_leveldb_metadata(char* name) {
    char *err = NULL;

    /******************************************/
    /* OPEN */

    leveldb_options_t * options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 1);
    db_metadata = leveldb_open(options, name, &err);
    leveldb_options_destroy(options);    

    if (err != NULL) {
      fprintf(stderr, "Open fail: %s\n", err);
      return(1);
    }
    else {
      printf("DB created\n");
    }

    /* reset error var */
    leveldb_free(err); 

    return(0);
}

int put_leveldb_metadata(const char* key, size_t keylen, const char* val, size_t vallen){
    char* err = NULL;

    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_put(db_metadata, woptions, key, keylen, val, vallen, &err);
    leveldb_writeoptions_destroy(woptions);

    if (err != NULL) {
      fprintf(stderr, "Write metadata fail.\n");
      return(1);
    }

    leveldb_free(err);

    return(0);
}

void get_leveldb_metadata(const char* key, size_t keylen, char * dest){
    char* read = NULL;
    size_t read_len;
    char* err = NULL;

    leveldb_readoptions_t *roptions = leveldb_readoptions_create();
    read = leveldb_get(db_metadata, roptions, key, keylen, &read_len, &err);
    leveldb_readoptions_destroy(roptions);

    if (err != NULL) {
      fprintf(stderr, "Read fail.\n");
      return;
    }
    
    if (read != NULL){
    	memcpy(dest, read, read_len);
	free(read);

    }
    
    leveldb_free(err);
}

int delete_leveldb_metadata(const char* key, size_t keylen){
    char* err = NULL;

    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_delete(db_metadata, woptions, key, keylen, &err);
    leveldb_writeoptions_destroy(woptions);

    if (err != NULL) {
      fprintf(stderr, "Delete fail.\n");
      return(1);
    }

    leveldb_free(err); 

    return(0);
}

void close_leveldb_metadata(){
    leveldb_close(db_metadata);
}


/***********************************INDEX****************************************/


int create_leveldb_index(char* name) {
    char *err = NULL;

    /******************************************/
    /* OPEN */

    leveldb_options_t *options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 1);
    db_index = leveldb_open(options, name, &err);
    leveldb_options_destroy(options);

    if (err != NULL) {
      fprintf(stderr, "Open fail: %s\n", err);
      return(1);
    }
    else {
      printf("DB created\n");
    }

    /* reset error var */
    leveldb_free(err); 

    return(0);
}

int put_leveldb_index(const char* key, size_t keylen, const char* val, size_t vallen){
    char* err = NULL;
    
    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_put(db_index, woptions, key, keylen, val, vallen, &err);
    leveldb_writeoptions_destroy(woptions);

    if (err != NULL) {
      fprintf(stderr, "Write index fail.\n");
      return(1);
    }

    leveldb_free(err); 

    return(0);
}

void get_leveldb_index(const char* key, size_t keylen, char * dest){
    char* read = NULL;
    size_t read_len;
    char* err = NULL;
    
    leveldb_readoptions_t *roptions = leveldb_readoptions_create();
    read = leveldb_get(db_index, roptions, key, keylen, &read_len, &err);
    leveldb_readoptions_destroy(roptions);

    if (err != NULL) {
      fprintf(stderr, "Read fail.\n");
      return;
    }
    
    if (read != NULL){
    	memcpy(dest, read, read_len);
	free(read);
    }

    leveldb_free(err); 
}

int delete_leveldb_index(const char* key, size_t keylen){
    char* err = NULL;

    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_delete(db_index, woptions, key, keylen, &err);
    leveldb_writeoptions_destroy(woptions);

    if (err != NULL) {
      fprintf(stderr, "Delete fail.\n");
      return(1);
    }

    leveldb_free(err); 

    return(0);
}

void close_leveldb_index(){
    leveldb_close(db_index);
}

/************************************DESTROY**********************************/

int destroy_leveldb(char* name){
    char *err = NULL;
    
    leveldb_options_t *options = leveldb_options_create();
    leveldb_destroy_db(options, name, &err);
    leveldb_options_destroy(options);

    if (err != NULL) {
      fprintf(stderr, "Destroy fail.\n");
      return(1);
    }

    leveldb_free(err); 

    return(0);
}

