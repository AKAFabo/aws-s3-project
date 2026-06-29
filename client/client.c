#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/protocol.h"

/* ─────────────────────────────────────────────
   CONNECT TO SERVER
   ───────────────────────────────────────────── */
static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

/* ─────────────────────────────────────────────
   RECEIVE AND PRINT SERVER RESPONSE
   If save_path != NULL, write payload bytes to that file.
   Returns resp.status (0=OK).
   ───────────────────────────────────────────── */
static int receive_response(int fd, const char *save_path) {
    ResponseHeader resp;
    if (recv_all(fd, &resp, sizeof(resp)) != 0) {
        fprintf(stderr, "client: failed to read server response\n");
        return -1;
    }

    if (resp.message[0]) printf("%s\n", resp.message);

    if (resp.data_len > 0) {
        if (save_path && save_path[0]) {
            /* Download to file */
            void *buf = malloc(resp.data_len);
            if (!buf) { fprintf(stderr, "out of memory\n"); return -1; }
            if (recv_all(fd, buf, resp.data_len) != 0) { free(buf); return -1; }
            FILE *f = fopen(save_path, "wb");
            if (!f) { perror(save_path); free(buf); return -1; }
            fwrite(buf, 1, resp.data_len, f);
            fclose(f);
            free(buf);
        } else {
            /* Print text payload */
            char *buf = malloc(resp.data_len + 1);
            if (!buf) return -1;
            recv_all(fd, buf, resp.data_len);
            buf[resp.data_len] = '\0';
            printf("%s", buf);
            free(buf);
        }
    }
    return resp.status;
}

/* ─────────────────────────────────────────────
   RECEIVE RESPONSE INTO A BUFFER (don't print)
   Used internally by sync to get key listings.
   Caller must free() *out_buf.
   ───────────────────────────────────────────── */
static int receive_response_buf(int fd, char **out_buf, uint64_t *out_len) {
    ResponseHeader resp;
    if (recv_all(fd, &resp, sizeof(resp)) != 0) return -1;
    if (resp.status != 0 || resp.data_len == 0) {
        *out_buf = NULL; *out_len = 0;
        return resp.status;
    }
    char *buf = malloc(resp.data_len + 1);
    if (!buf) return -1;
    recv_all(fd, buf, resp.data_len);
    buf[resp.data_len] = '\0';
    *out_buf = buf;
    *out_len = resp.data_len;
    return 0;
}

/* ─────────────────────────────────────────────
   UPLOAD ONE LOCAL FILE → S3
   Opens a new connection per file (simple & reliable).
   key_override: if set, use this as the S3 key instead
                 of deriving it from the filename.
   ───────────────────────────────────────────── */
static int upload_one(const char *local_path, const char *s3_dst,
                      const char *key_override) {
    struct stat st;
    if (stat(local_path, &st) != 0) { perror(local_path); return -1; }

    RequestHeader req;
    memset(&req, 0, sizeof(req));
    req.cmd      = CMD_CP;
    req.data_len = (uint64_t)st.st_size;
    strncpy(req.src, local_path, MAX_KEY_LEN - 1);

    if (key_override && key_override[0]) {
        /* Build full s3://bucket/key destination */
        snprintf(req.dst, MAX_KEY_LEN, "%s%s", s3_dst, key_override);
    } else {
        strncpy(req.dst, s3_dst, MAX_KEY_LEN - 1);
    }

    int fd = connect_to_server();
    if (fd < 0) return -1;

    /* Send header */
    if (send_all(fd, &req, sizeof(req)) != 0) { close(fd); return -1; }

    /* Stream file */
    FILE *f = fopen(local_path, "rb");
    if (!f) { perror(local_path); close(fd); return -1; }
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        send_all(fd, chunk, n);
    fclose(f);

    int ret = receive_response(fd, NULL);
    close(fd);
    return ret;
}

/* ─────────────────────────────────────────────
   DOWNLOAD ONE S3 KEY → LOCAL FILE
   Creates intermediate directories as needed.
   ───────────────────────────────────────────── */
static int download_one(const char *s3_src_bucket, const char *key,
                        const char *local_dst_dir) {
    /* Build local path: dst_dir/key  (key may contain slashes) */
    char local_path[1024];
    snprintf(local_path, sizeof(local_path), "%s/%s", local_dst_dir, key);

    /* Create any missing parent directories */
    char tmp[1024];
    strncpy(tmp, local_path, sizeof(tmp) - 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }

    /* Build s3://bucket/key source */
    char s3_src[MAX_KEY_LEN];
    snprintf(s3_src, sizeof(s3_src), "s3://%s/%s", s3_src_bucket, key);

    RequestHeader req;
    memset(&req, 0, sizeof(req));
    req.cmd = CMD_CP;
    strncpy(req.src, s3_src,     MAX_KEY_LEN - 1);
    strncpy(req.dst, local_path, MAX_KEY_LEN - 1);

    int fd = connect_to_server();
    if (fd < 0) return -1;
    send_all(fd, &req, sizeof(req));
    int ret = receive_response(fd, local_path);
    close(fd);
    return ret;
}

/* ─────────────────────────────────────────────
   GET KEY LISTING FROM SERVER
   Sends CMD_LIST_KEYS and returns a newline-separated
   list of "key\tsize" pairs.  Caller must free().
   ───────────────────────────────────────────── */
static char *fetch_key_list(const char *bucket, const char *prefix) {
    RequestHeader req;
    memset(&req, 0, sizeof(req));
    req.cmd = CMD_LIST_KEYS;
    snprintf(req.src, MAX_KEY_LEN, "s3://%s/%s", bucket, prefix ? prefix : "");

    int fd = connect_to_server();
    if (fd < 0) return NULL;
    send_all(fd, &req, sizeof(req));

    char *buf = NULL; uint64_t len = 0;
    receive_response_buf(fd, &buf, &len);
    close(fd);
    return buf;   /* may be NULL if bucket empty */
}

/* ─────────────────────────────────────────────
   RECURSIVE CP  local-dir → s3://bucket/prefix
   Walks local_dir recursively, uploading every file.
   rel_prefix: the key prefix to prepend (the path
               relative to the top-level src dir).
   ───────────────────────────────────────────── */
static int cp_recursive_up(const char *local_dir, const char *s3_dst,
                            const char *rel_prefix) {
    DIR *d = opendir(local_dir);
    if (!d) { perror(local_dir); return -1; }

    struct dirent *entry;
    int errors = 0;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", local_dir, entry->d_name);

        /* Key = rel_prefix + filename */
        char key[MAX_KEY_LEN];
        if (rel_prefix && rel_prefix[0])
            snprintf(key, sizeof(key), "%s/%s", rel_prefix, entry->d_name);
        else
            strncpy(key, entry->d_name, sizeof(key) - 1);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            errors += cp_recursive_up(full_path, s3_dst, key);
        } else if (S_ISREG(st.st_mode)) {
            /* Only upload regular files (skip symlinks, devices, etc.) */
            if (upload_one(full_path, s3_dst, key) != 0) errors++;
        }
        /* Skip anything else (symlinks, sockets, etc.) */
    }
    closedir(d);
    return errors;
}

/* ─────────────────────────────────────────────
   RECURSIVE CP  s3://bucket/prefix → local-dir
   Fetches key list from server, downloads each one.
   ───────────────────────────────────────────── */
static int cp_recursive_down(const char *bucket, const char *prefix,
                              const char *local_dst) {
    char *listing = fetch_key_list(bucket, prefix);
    if (!listing) {
        printf("(no objects found under prefix '%s')\n", prefix ? prefix : "");
        return 0;
    }

    int errors = 0;
    char *line = strtok(listing, "\n");
    while (line) {
        /* Each line: "key\tsize" */
        char *tab = strchr(line, '\t');
        if (tab) *tab = '\0';   /* key is now just line */
        if (line[0]) {
            printf("download: s3://%s/%s → %s/%s\n", bucket, line, local_dst, line);
            if (download_one(bucket, line, local_dst) != 0) errors++;
        }
        line = strtok(NULL, "\n");
    }
    free(listing);
    return errors;
}

/* ─────────────────────────────────────────────
   SYNC  local-dir → s3://bucket/prefix
   Only uploads files that are new or size-changed.
   With --delete: also removes S3 keys that don't
   exist locally anymore.
   ───────────────────────────────────────────── */

/* Helper: collect all local files into a flat list */
typedef struct { char rel_path[MAX_KEY_LEN]; uint64_t size; } LocalFile;
static int collect_local(const char *base_dir, const char *rel,
                          LocalFile *files, int *count, int max) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", base_dir, rel[0] ? rel : ".");
    /* trim trailing "/." */
    if (dir[strlen(dir)-1] == '.' && dir[strlen(dir)-2] == '/')
        dir[strlen(dir)-2] = '\0';

    DIR *d = opendir(dir[0] ? dir : base_dir);
    if (!d) return -1;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;

        char full[1024];
        snprintf(full, sizeof(full), "%s/%s",
                 dir[0] ? dir : base_dir, entry->d_name);

        char rel_child[MAX_KEY_LEN];
        if (rel[0])
            snprintf(rel_child, sizeof(rel_child), "%s/%s", rel, entry->d_name);
        else
            strncpy(rel_child, entry->d_name, sizeof(rel_child) - 1);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            collect_local(base_dir, rel_child, files, count, max);
        } else if (S_ISREG(st.st_mode) && *count < max) {
            strncpy(files[*count].rel_path, rel_child, MAX_KEY_LEN - 1);
            files[*count].size = (uint64_t)st.st_size;
            (*count)++;
        }
    }
    closedir(d);
    return 0;
}

static int do_sync(const char *local_dir, const char *bucket,
                   const char *prefix, int delete_flag) {
    /* 1. Get all S3 keys under prefix */
    char *listing = fetch_key_list(bucket, prefix);

    /* Parse listing into a simple key→size map */
    /* Format per line: "key\tsize\n" */
    typedef struct { char key[MAX_KEY_LEN]; uint64_t size; } S3Entry;
    S3Entry *s3_entries = calloc(MAX_ENTRIES, sizeof(S3Entry));
    int s3_count = 0;

    if (listing) {
        char *line = strtok(listing, "\n");
        while (line && s3_count < MAX_ENTRIES) {
            char *tab = strchr(line, '\t');
            if (tab) {
                *tab = '\0';
                strncpy(s3_entries[s3_count].key,  line,    MAX_KEY_LEN - 1);
                s3_entries[s3_count].size = (uint64_t)strtoull(tab + 1, NULL, 10);
                s3_count++;
            }
            line = strtok(NULL, "\n");
        }
        free(listing);
    }

    /* 2. Collect local files */
    LocalFile *local_files = calloc(MAX_ENTRIES, sizeof(LocalFile));
    int local_count = 0;
    collect_local(local_dir, "", local_files, &local_count, MAX_ENTRIES);

    int uploaded = 0, skipped = 0, deleted = 0;

    /* 3. Upload new or changed files */
    for (int i = 0; i < local_count; i++) {
        const char *rel = local_files[i].rel_path;
        uint64_t local_size = local_files[i].size;

        /* Build the full S3 key (prefix + rel) */
        char s3_key[MAX_KEY_LEN];
        if (prefix && prefix[0])
            snprintf(s3_key, sizeof(s3_key), "%s/%s", prefix, rel);
        else
            strncpy(s3_key, rel, sizeof(s3_key) - 1);

        /* Check if already exists with same size on S3 */
        int found = 0;
        for (int j = 0; j < s3_count; j++) {
            if (strcmp(s3_entries[j].key, s3_key) == 0) {
                if (s3_entries[j].size == local_size) {
                    skipped++;
                    found = 1;
                }
                break;
            }
        }
        if (!found) {
            char local_path[1024];
            snprintf(local_path, sizeof(local_path), "%s/%s", local_dir, rel);
            char s3_dst[MAX_KEY_LEN];
            snprintf(s3_dst, sizeof(s3_dst), "s3://%s/", bucket);
            printf("sync upload: %s → s3://%s/%s\n", local_path, bucket, s3_key);
            if (upload_one(local_path, s3_dst, s3_key) == 0) uploaded++;
        }
    }

    /* 4. --delete: remove S3 keys that are no longer local */
    if (delete_flag) {
        for (int j = 0; j < s3_count; j++) {
            /* Strip prefix from s3 key to get the rel path */
            const char *s3_key = s3_entries[j].key;
            const char *rel = s3_key;
            if (prefix && prefix[0] && strncmp(s3_key, prefix, strlen(prefix)) == 0)
                rel = s3_key + strlen(prefix) + 1;

            /* Check if rel exists locally */
            int found = 0;
            for (int i = 0; i < local_count; i++) {
                if (strcmp(local_files[i].rel_path, rel) == 0) {
                    found = 1; break;
                }
            }
            if (!found) {
                printf("sync delete: s3://%s/%s\n", bucket, s3_key);
                RequestHeader req;
                memset(&req, 0, sizeof(req));
                req.cmd = CMD_RM;
                snprintf(req.src, MAX_KEY_LEN, "s3://%s/%s", bucket, s3_key);
                int fd = connect_to_server();
                if (fd >= 0) {
                    send_all(fd, &req, sizeof(req));
                    receive_response(fd, NULL);
                    close(fd);
                    deleted++;
                }
            }
        }
    }

    printf("sync complete: %d uploaded, %d skipped (unchanged), %d deleted\n",
           uploaded, skipped, deleted);

    free(s3_entries);
    free(local_files);
    return 0;
}

/* ─────────────────────────────────────────────
   PARSE s3://bucket/key  →  bucket + key
   ───────────────────────────────────────────── */
static void parse_s3(const char *s3path,
                     char *bucket, size_t blen,
                     char *key,    size_t klen) {
    const char *p = s3path;
    if (strncmp(p, "s3://", 5) == 0) p += 5;
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= blen) n = blen - 1;
        strncpy(bucket, p, n); bucket[n] = '\0';
        strncpy(key, slash + 1, klen - 1); key[klen-1] = '\0';
    } else {
        strncpy(bucket, p, blen - 1); bucket[blen-1] = '\0';
        key[0] = '\0';
    }
}

/* ─────────────────────────────────────────────
   USAGE
   ───────────────────────────────────────────── */
static void print_usage(void) {
    printf("Usage:\n");
    printf("  aws-s3 ls [s3://bucket/prefix]\n");
    printf("  aws-s3 mb s3://bucket\n");
    printf("  aws-s3 rb s3://bucket [--force]\n");
    printf("  aws-s3 cp <src> <dst> [--recursive]\n");
    printf("  aws-s3 mv <src> <dst> [--recursive]\n");
    printf("  aws-s3 rm s3://bucket/key [--recursive]\n");
    printf("  aws-s3 sync <local-dir> s3://bucket[/prefix] [--delete]\n");
    printf("  aws-s3 sync s3://bucket[/prefix] <local-dir>\n");
}

/* ─────────────────────────────────────────────
   MAIN
   ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(); return 1; }

    const char *subcmd = argv[1];
    int recursive = 0, force = 0, delete_flag = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) recursive   = 1;
        if (strcmp(argv[i], "--force")     == 0) force       = 1;
        if (strcmp(argv[i], "--delete")    == 0) delete_flag = 1;
    }

    /* ── ls ── */
    if (strcmp(subcmd, "ls") == 0) {
        RequestHeader req; memset(&req, 0, sizeof(req));
        req.cmd = CMD_LS;
        if (argc >= 3 && argv[2][0] != '-') strncpy(req.src, argv[2], MAX_KEY_LEN-1);
        int fd = connect_to_server(); if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    /* ── mb ── */
    } else if (strcmp(subcmd, "mb") == 0) {
        if (argc < 3) { fprintf(stderr, "mb: missing bucket\n"); return 1; }
        RequestHeader req; memset(&req, 0, sizeof(req));
        req.cmd = CMD_MB;
        strncpy(req.src, argv[2], MAX_KEY_LEN-1);
        int fd = connect_to_server(); if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    /* ── rb ── */
    } else if (strcmp(subcmd, "rb") == 0) {
        if (argc < 3) { fprintf(stderr, "rb: missing bucket\n"); return 1; }
        RequestHeader req; memset(&req, 0, sizeof(req));
        req.cmd = CMD_RB; req.force = force;
        strncpy(req.src, argv[2], MAX_KEY_LEN-1);
        int fd = connect_to_server(); if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    /* ── cp ── */
    } else if (strcmp(subcmd, "cp") == 0) {
        if (argc < 4) { fprintf(stderr, "cp: need src and dst\n"); return 1; }
        const char *src = argv[2], *dst = argv[3];
        int src_is_s3 = (strncmp(src, "s3://", 5) == 0);
        int dst_is_s3 = (strncmp(dst, "s3://", 5) == 0);

        if (recursive && !src_is_s3 && dst_is_s3) {
            /* Upload directory recursively */
            cp_recursive_up(src, dst, "");
        } else if (recursive && src_is_s3 && !dst_is_s3) {
            /* Download all keys under prefix */
            char bucket[256]={0}, prefix[MAX_KEY_LEN]={0};
            parse_s3(src, bucket, sizeof(bucket), prefix, sizeof(prefix));
            cp_recursive_down(bucket, prefix, dst);
        } else {
            /* Single file cp (original logic) */
            RequestHeader req; memset(&req, 0, sizeof(req));
            req.cmd = CMD_CP;
            strncpy(req.src, src, MAX_KEY_LEN-1);
            strncpy(req.dst, dst, MAX_KEY_LEN-1);
            int fd = connect_to_server(); if (fd < 0) return 1;
            if (!src_is_s3) {
                /* Upload */
                struct stat st;
                if (stat(src, &st) != 0) { perror(src); close(fd); return 1; }
                req.data_len = (uint64_t)st.st_size;
                send_all(fd, &req, sizeof(req));
                FILE *f = fopen(src, "rb");
                char chunk[65536]; size_t n;
                while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
                    send_all(fd, chunk, n);
                fclose(f);
                receive_response(fd, NULL);
            } else {
                send_all(fd, &req, sizeof(req));
                const char *save = dst_is_s3 ? NULL : dst;
                receive_response(fd, save);
            }
            close(fd);
        }

    /* ── mv ── */
    } else if (strcmp(subcmd, "mv") == 0) {
        if (argc < 4) { fprintf(stderr, "mv: need src and dst\n"); return 1; }
        const char *src = argv[2], *dst = argv[3];
        int src_is_s3 = (strncmp(src, "s3://", 5) == 0);
        int dst_is_s3 = (strncmp(dst, "s3://", 5) == 0);

        if (recursive && !src_is_s3 && dst_is_s3) {
            /* Upload directory then delete local files */
            if (cp_recursive_up(src, dst, "") == 0) {
                /* Remove local dir after successful upload */
                /* (simple: user can rm -rf; out of scope for this project) */
                printf("mv: local directory uploaded. Remove local dir manually.\n");
            }
        } else if (recursive && src_is_s3 && !dst_is_s3) {
            char bucket[256]={0}, prefix[MAX_KEY_LEN]={0};
            parse_s3(src, bucket, sizeof(bucket), prefix, sizeof(prefix));
            if (cp_recursive_down(bucket, prefix, dst) == 0) {
                /* Delete all keys under prefix on S3 */
                RequestHeader req; memset(&req, 0, sizeof(req));
                req.cmd = CMD_RM; req.recursive = 1;
                strncpy(req.src, src, MAX_KEY_LEN-1);
                int fd = connect_to_server();
                if (fd >= 0) { send_all(fd, &req, sizeof(req)); receive_response(fd, NULL); close(fd); }
            }
        } else {
            /* Single file mv */
            RequestHeader req; memset(&req, 0, sizeof(req));
            req.cmd = CMD_MV;
            strncpy(req.src, src, MAX_KEY_LEN-1);
            strncpy(req.dst, dst, MAX_KEY_LEN-1);
            int fd = connect_to_server(); if (fd < 0) return 1;
            if (!src_is_s3) {
                struct stat st; stat(src, &st);
                req.data_len = (uint64_t)st.st_size;
                send_all(fd, &req, sizeof(req));
                FILE *f = fopen(src, "rb");
                char chunk[65536]; size_t n;
                while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
                    send_all(fd, chunk, n);
                fclose(f);
                receive_response(fd, NULL);
                remove(src);
            } else {
                send_all(fd, &req, sizeof(req));
                const char *save = dst_is_s3 ? NULL : dst;
                receive_response(fd, save);
            }
            close(fd);
        }

    /* ── rm ── */
    } else if (strcmp(subcmd, "rm") == 0) {
        if (argc < 3) { fprintf(stderr, "rm: missing path\n"); return 1; }
        RequestHeader req; memset(&req, 0, sizeof(req));
        req.cmd = CMD_RM; req.recursive = recursive;
        strncpy(req.src, argv[2], MAX_KEY_LEN-1);
        int fd = connect_to_server(); if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    /* ── sync ── */
    } else if (strcmp(subcmd, "sync") == 0) {
        if (argc < 4) { fprintf(stderr, "sync: need src and dst\n"); return 1; }
        const char *src = argv[2], *dst = argv[3];
        int src_is_s3 = (strncmp(src, "s3://", 5) == 0);
        int dst_is_s3 = (strncmp(dst, "s3://", 5) == 0);

        if (!src_is_s3 && dst_is_s3) {
            /* local → S3 */
            char bucket[256]={0}, prefix[MAX_KEY_LEN]={0};
            parse_s3(dst, bucket, sizeof(bucket), prefix, sizeof(prefix));
            do_sync(src, bucket, prefix, delete_flag);
        } else if (src_is_s3 && !dst_is_s3) {
            /* S3 → local (download all, no --delete support for now) */
            char bucket[256]={0}, prefix[MAX_KEY_LEN]={0};
            parse_s3(src, bucket, sizeof(bucket), prefix, sizeof(prefix));
            cp_recursive_down(bucket, prefix, dst);
        } else {
            fprintf(stderr, "sync: one side must be s3://, the other local\n");
            return 1;
        }

    } else {
        fprintf(stderr, "Unknown command: %s\n", subcmd);
        print_usage();
        return 1;
    }

    return 0;
}
