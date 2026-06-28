#ifndef BUCKET_H
#define BUCKET_H

#include "../common/protocol.h"

/*
 * bucket.h — all functions that read/write bucket files.
 *
 * A bucket file has this layout on disk:
 *
 *   [ DirBlock (fixed size, always at byte 0)        ]
 *   [ file contents packed sequentially after that   ]
 *
 * The DirBlock lists every object stored in this bucket,
 * with the byte offset where that object's data starts.
 * Free slots (from deleted or replaced files) are also
 * tracked inside the DirBlock so space can be reused.
 */

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

/*
 * Store a file into a bucket.
 *   bucket_name : name of the target bucket
 *   key         : object key (the "path" inside the bucket)
 *   data        : pointer to file bytes
 *   size        : number of bytes
 * Returns 0 on success.
 */
int  bucket_put(const char *bucket_name, const char *key,
                const void *data, uint64_t size);

/*
 * Retrieve a file from a bucket.
 *   bucket_name : name of the source bucket
 *   key         : object key to look up
 *   out_size    : set to the object's byte count on success
 * Returns a malloc'd buffer with the object data (caller must free),
 * or NULL if not found.
 */
void *bucket_get(const char *bucket_name, const char *key,
                 uint64_t *out_size);

/* Delete one object from a bucket. Returns 0 on success. */
int  bucket_rm(const char *bucket_name, const char *key);

/* Low-level helpers (used internally) */
int  bucket_read_dirblock(const char *bucket_name, DirBlock *block);
int  bucket_write_dirblock(const char *bucket_name, const DirBlock *block);
char *bucket_filepath(const char *bucket_name); /* returns path to .bucket file */

#endif /* BUCKET_H */
