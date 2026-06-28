#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ─────────────────────────────────────────────
   NETWORK SETTINGS
   ───────────────────────────────────────────── */
#define SERVER_PORT     8080
#define SERVER_HOST     "127.0.0.1"
#define BACKLOG         10          /* max pending connections */

/* ─────────────────────────────────────────────
   BUCKET FILE LIMITS
   ───────────────────────────────────────────── */
#define MAX_KEY_LEN     512         /* max length of an object key (path) */
#define MAX_ENTRIES     1024        /* max files per bucket               */
#define BUCKET_DIR      "./buckets" /* folder where bucket files are saved */

/* ─────────────────────────────────────────────
   COMMAND TYPES
   Sent from client → server to identify the operation.
   ───────────────────────────────────────────── */
typedef enum {
    CMD_LS   = 1,   /* list buckets or bucket contents */
    CMD_MB   = 2,   /* make bucket                     */
    CMD_RB   = 3,   /* remove bucket                   */
    CMD_CP   = 4,   /* copy file                       */
    CMD_MV   = 5,   /* move file                       */
    CMD_RM   = 6,   /* remove object                   */
    CMD_SYNC = 7    /* sync directory with bucket      */
} CommandType;

/* ─────────────────────────────────────────────
   REQUEST HEADER
   Every message from client → server starts with this.
   Followed by `data_len` bytes of payload (if any).
   ───────────────────────────────────────────── */
typedef struct {
    CommandType cmd;            /* which command                        */
    char   src[MAX_KEY_LEN];    /* source path  (local or s3://)        */
    char   dst[MAX_KEY_LEN];    /* dest path    (local or s3://)        */
    int    recursive;           /* 1 if --recursive flag was passed     */
    int    force;               /* 1 if --force flag was passed         */
    int    delete_flag;         /* 1 if --delete flag was passed        */
    uint64_t data_len;          /* bytes of file payload that follow    */
} RequestHeader;

/* ─────────────────────────────────────────────
   RESPONSE HEADER
   Every message from server → client starts with this.
   Followed by `data_len` bytes of payload (if any).
   ───────────────────────────────────────────── */
typedef struct {
    int      status;            /* 0 = OK, non-zero = error             */
    char     message[256];      /* human-readable status message        */
    uint64_t data_len;          /* bytes of file payload that follow    */
} ResponseHeader;

/* ─────────────────────────────────────────────
   BUCKET DIRECTORY ENTRY
   Stored at the top of every bucket file.
   Describes one object stored inside that bucket.
   ───────────────────────────────────────────── */
typedef struct {
    char     key[MAX_KEY_LEN];  /* object key e.g. "fotos/playa.jpg"   */
    int64_t  offset;            /* byte offset inside the bucket file   */
    uint64_t size;              /* size of this object in bytes         */
    int      is_free;           /* 1 = this slot is free space, not obj */
} DirEntry;

/* ─────────────────────────────────────────────
   BUCKET DIRECTORY BLOCK
   Written at byte 0 of every bucket file.
   After this block, file contents follow sequentially.
   ───────────────────────────────────────────── */
typedef struct {
    int      entry_count;               /* number of entries in use     */
    DirEntry entries[MAX_ENTRIES];      /* the entries themselves        */
} DirBlock;

/* ─────────────────────────────────────────────
   HELPER: safely send/receive exact byte counts
   (declared here, implemented in protocol.c)
   ───────────────────────────────────────────── */
int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void       *buf, size_t len);

#endif /* PROTOCOL_H */
