#include "bucket.h"
#include "../common/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>


char *bucket_filepath(const char *bucket_name) {
    /* +16 for the directory prefix, extension, and null terminator */
    size_t len = strlen(BUCKET_DIR) + strlen(bucket_name) + 16;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/%s.bucket", BUCKET_DIR, bucket_name);
    return path;   /* caller must free() */
}


int bucket_exists(const char *bucket_name) {
    char *path = bucket_filepath(bucket_name);
    if (!path) return 0;
    struct stat st;
    int exists = (stat(path, &st) == 0);
    free(path);
    return exists;
}


int bucket_read_dirblock(const char *bucket_name, DirBlock *block) {
    char *path = bucket_filepath(bucket_name);
    if (!path) return -1;

    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return -1;

    size_t n = fread(block, sizeof(DirBlock), 1, f);
    fclose(f);
    return (n == 1) ? 0 : -1;
}


int bucket_write_dirblock(const char *bucket_name, const DirBlock *block) {
    char *path = bucket_filepath(bucket_name);
    if (!path) return -1;

    /* "r+b" = open existing file for reading+writing, no truncate */
    FILE *f = fopen(path, "r+b");
    free(path);
    if (!f) return -1;

    rewind(f);  /* go to byte 0 */
    size_t n = fwrite(block, sizeof(DirBlock), 1, f);
    fclose(f);
    return (n == 1) ? 0 : -1;
}


int bucket_create(const char *bucket_name) {
    /* Make sure the buckets directory exists */
    mkdir(BUCKET_DIR, 0755);    /* ignore error if already exists */

    if (bucket_exists(bucket_name)) {
        fprintf(stderr, "bucket_create: '%s' already exists\n", bucket_name);
        return -1;
    }

    char *path = bucket_filepath(bucket_name);
    if (!path) return -1;

    FILE *f = fopen(path, "wb");
    free(path);
    if (!f) return -1;

    /* Write an empty DirBlock to reserve space at byte 0 */
    DirBlock block;
    memset(&block, 0, sizeof(DirBlock));
    fwrite(&block, sizeof(DirBlock), 1, f);
    fclose(f);
    return 0;
}

int bucket_delete(const char *bucket_name, int force) {
    if (!bucket_exists(bucket_name)) return -1;

    if (!force) {
        /* Check if bucket is empty before deleting */
        DirBlock block;
        if (bucket_read_dirblock(bucket_name, &block) == 0) {
            /* Count active (non-free) entries */
            int active = 0;
            for (int i = 0; i < block.entry_count; i++) {
                if (!block.entries[i].is_free) active++;
            }
            if (active > 0) {
                fprintf(stderr, "bucket_delete: bucket '%s' is not empty (use --force)\n",
                        bucket_name);
                return -1;
            }
        }
    }

    char *path = bucket_filepath(bucket_name);
    if (!path) return -1;
    int ret = remove(path);
    free(path);
    return ret;
}


void bucket_list_all(int client_fd) {
    DIR *dir = opendir(BUCKET_DIR);
    if (!dir) {
        /* No buckets directory yet — send empty response */
        ResponseHeader resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "(no buckets found)");
        resp.data_len = 0;
        send_all(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Build a text listing of all .bucket files */
    char listing[8192] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Only list files that end in ".bucket" */
        char *dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".bucket") == 0) {
            /* Print just the bucket name (strip the .bucket extension) */
            char name[256];
            size_t namelen = (size_t)(dot - entry->d_name);
            strncpy(name, entry->d_name, namelen);
            name[namelen] = '\0';
            strncat(listing, "  s3://", sizeof(listing) - strlen(listing) - 1);
            strncat(listing, name,     sizeof(listing) - strlen(listing) - 1);
            strncat(listing, "\n",     sizeof(listing) - strlen(listing) - 1);
        }
    }
    closedir(dir);

    /* Send response header + listing text */
    ResponseHeader resp;
    memset(&resp, 0, sizeof(resp));
    resp.status   = 0;
    resp.data_len = strlen(listing);
    snprintf(resp.message, sizeof(resp.message), "OK");
    send_all(client_fd, &resp, sizeof(resp));
    if (resp.data_len > 0)
        send_all(client_fd, listing, resp.data_len);
}


void bucket_list(int client_fd, const char *bucket_name, const char *prefix) {
    ResponseHeader resp;
    memset(&resp, 0, sizeof(resp));

    if (!bucket_exists(bucket_name)) {
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message),
                 "Error: bucket '%s' does not exist", bucket_name);
        send_all(client_fd, &resp, sizeof(resp));
        return;
    }

    DirBlock block;
    if (bucket_read_dirblock(bucket_name, &block) != 0) {
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "Error: could not read bucket");
        send_all(client_fd, &resp, sizeof(resp));
        return;
    }

    char listing[32768] = {0};
    for (int i = 0; i < block.entry_count; i++) {
        DirEntry *e = &block.entries[i];
        if (e->is_free) continue;
        /* Filter by prefix if provided */
        if (prefix && prefix[0] && strncmp(e->key, prefix, strlen(prefix)) != 0)
            continue;
        char line[600];
        snprintf(line, sizeof(line), "  %s  (%llu bytes)\n",
                 e->key, (unsigned long long)e->size);
        strncat(listing, line, sizeof(listing) - strlen(listing) - 1);
    }

    resp.status   = 0;
    resp.data_len = strlen(listing);
    if (resp.data_len == 0)
        snprintf(resp.message, sizeof(resp.message), "(no objects found)");
    else
        snprintf(resp.message, sizeof(resp.message), "OK");
    send_all(client_fd, &resp, sizeof(resp));
    if (resp.data_len > 0)
        send_all(client_fd, listing, resp.data_len);
}


int bucket_put(const char *bucket_name, const char *key,
               const void *data, uint64_t size) {
    if (!bucket_exists(bucket_name)) return -1;

    DirBlock block;
    if (bucket_read_dirblock(bucket_name, &block) != 0) return -1;

    char *path = bucket_filepath(bucket_name);
    if (!path) return -1;

    /* ── Check if key already e
    typedef struct { char key[MAX_KEY_LExists ── */
    for (int i = 0; i < block.entry_count; i++) {
        DirEntry *e = &block.entries[i];
        if (!e->is_free && strcmp(e->key, key) == 0) {
            if (e->size == size) {
                /* Same size: overwrite in place */
                FILE *f = fopen(path, "r+b");
                free(path);
                if (!f) return -1;
                fseeko(f, (off_t)e->offset, SEEK_SET);
                fwrite(data, 1, size, f);
                fclose(f);
                return 0;
            } else {
                /* Different size: mark old slot as free */
                e->is_free = 1;
                /* (the key is cleared so it looks empty) */
                memset(e->key, 0, MAX_KEY_LEN);
                break;
            }
        }
    }

    /* ── Find a free slot using First-Fit ── */
    int    reuse_idx = -1;
    for (int i = 0; i < block.entry_count; i++) {
        if (block.entries[i].is_free && block.entries[i].size >= size) {
            reuse_idx = i;
            break;
        }
    }

    FILE *f = fopen(path, "r+b");
    free(path);
    if (!f) return -1;

    int64_t write_offset;

    if (reuse_idx >= 0) {
        /* Reuse a freed slot */
        write_offset = block.entries[reuse_idx].offset;
        /* Clear the old free entry */
        memset(&block.entries[reuse_idx], 0, sizeof(DirEntry));
        block.entries[reuse_idx].is_free = 0;
    } else {
        /* Append to end of file */
        fseeko(f, 0, SEEK_END);
        write_offset = (int64_t)ftello(f);
    }

    /* Write file data */
    fseeko(f, (off_t)write_offset, SEEK_SET);
    fwrite(data, 1, size, f);
    fclose(f);

    /* Add new directory entry */
    if (reuse_idx >= 0) {
        strncpy(block.entries[reuse_idx].key, key, MAX_KEY_LEN - 1);
        block.entries[reuse_idx].offset  = write_offset;
        block.entries[reuse_idx].size    = size;
        block.entries[reuse_idx].is_free = 0;
    } else {
        if (block.entry_count >= MAX_ENTRIES) return -1;
        DirEntry *ne = &block.entries[block.entry_count];
        strncpy(ne->key, key, MAX_KEY_LEN - 1);
        ne->offset  = write_offset;
        ne->size    = size;
        ne->is_free = 0;
        block.entry_count++;
    }

    return bucket_write_dirblock(bucket_name, &block);
}


void *bucket_get(const char *bucket_name, const char *key, uint64_t *out_size) {
    if (!bucket_exists(bucket_name)) return NULL;

    DirBlock block;
    if (bucket_read_dirblock(bucket_name, &block) != 0) return NULL;

    for (int i = 0; i < block.entry_count; i++) {
        DirEntry *e = &block.entries[i];
        if (!e->is_free && strcmp(e->key, key) == 0) {
            char *path = bucket_filepath(bucket_name);
            if (!path) return NULL;

            FILE *f = fopen(path, "rb");
            free(path);
            if (!f) return NULL;

            void *buf = malloc(e->size);
            if (!buf) { fclose(f); return NULL; }

            fseeko(f, (off_t)e->offset, SEEK_SET);
            fread(buf, 1, e->size, f);
            fclose(f);

            *out_size = e->size;
            return buf;
        }
    }
    return NULL;   /* not found */
}


int bucket_rm(const char *bucket_name, const char *key) {
    if (!bucket_exists(bucket_name)) return -1;

    DirBlock block;
    if (bucket_read_dirblock(bucket_name, &block) != 0) return -1;

    for (int i = 0; i < block.entry_count; i++) {
        DirEntry *e = &block.entries[i];
        if (!e->is_free && strcmp(e->key, key) == 0) {
            /* Mark the slot as free (space is reclaimed later) */
            e->is_free = 1;
            memset(e->key, 0, MAX_KEY_LEN);
            return bucket_write_dirblock(bucket_name, &block);
        }
    }
    return -1;   /* key not found */
}
