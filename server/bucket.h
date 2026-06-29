#ifndef BUCKET_H
#define BUCKET_H

#include "../common/protocol.h"



/* Create a new, empty bucket file. Returns 0 on success. */
int  bucket_create(const char *bucket_name);

/* Delete a bucket file entirely. Returns 0 on success. */
int  bucket_delete(const char *bucket_name, int force);

/* Check whether a bucket exists. Returns 1 if yes, 0 if no. */
int  bucket_exists(const char *bucket_name);

/* List all buckets (prints to stdout). */
void bucket_list_all(int client_fd);

/* List all objects inside a bucket, optionally under a prefix. */
void bucket_list(int client_fd, const char *bucket_name, const char *prefix);


int  bucket_put(const char *bucket_name, const char *key,
                const void *data, uint64_t size);

void *bucket_get(const char *bucket_name, const char *key,
                 uint64_t *out_size);

/* Delete one object from a bucket. Returns 0 on success. */
int  bucket_rm(const char *bucket_name, const char *key);

/* Low-level helpers (used internally) */
int  bucket_read_dirblock(const char *bucket_name, DirBlock *block);
int  bucket_write_dirblock(const char *bucket_name, const DirBlock *block);
char *bucket_filepath(const char *bucket_name); /* returns path to .bucket file */

#endif /* BUCKET_H */
