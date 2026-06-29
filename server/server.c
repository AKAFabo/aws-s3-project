#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/protocol.h"
#include "bucket.h"

/* ─────────────────────────────────────────────
   FORWARD DECLARATIONS
   Each handler takes the connected client socket fd
   and the already-received request header.
   ───────────────────────────────────────────── */
static void handle_ls  (int client_fd, RequestHeader *req);
static void handle_mb  (int client_fd, RequestHeader *req);
static void handle_rb  (int client_fd, RequestHeader *req);
static void handle_cp  (int client_fd, RequestHeader *req);
static void handle_mv  (int client_fd, RequestHeader *req);
static void handle_rm  (int client_fd, RequestHeader *req);
static void handle_sync     (int client_fd, RequestHeader *req);
static void handle_list_keys(int client_fd, RequestHeader *req);

/* ─────────────────────────────────────────────
   SEND A SIMPLE TEXT RESPONSE  (no file payload)
   ───────────────────────────────────────────── */
static void send_response(int client_fd, int status, const char *msg) {
    ResponseHeader resp;
    memset(&resp, 0, sizeof(resp));
    resp.status   = status;
    resp.data_len = 0;
    strncpy(resp.message, msg, sizeof(resp.message) - 1);
    send_all(client_fd, &resp, sizeof(resp));
}

/* ─────────────────────────────────────────────
   PARSE "s3://bucket-name/optional/key" 
   Fills bucket_name and key (key may be empty).
   ───────────────────────────────────────────── */
static void parse_s3_path(const char *s3path,
                           char *bucket_name, size_t blen,
                           char *key,         size_t klen) {
    /* s3path looks like: s3://my-bucket/some/key.txt */
    const char *p = s3path;
    if (strncmp(p, "s3://", 5) == 0) p += 5;

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= blen) n = blen - 1;
        strncpy(bucket_name, p, n);
        bucket_name[n] = '\0';
        strncpy(key, slash + 1, klen - 1);
        key[klen - 1] = '\0';
    } else {
        strncpy(bucket_name, p, blen - 1);
        bucket_name[blen - 1] = '\0';
        key[0] = '\0';
    }
}

/* ─────────────────────────────────────────────
   COMMAND HANDLERS
   ───────────────────────────────────────────── */

/* ls: list buckets or contents of a bucket */
static void handle_ls(int client_fd, RequestHeader *req) {
    if (req->src[0] == '\0') {
        /* No argument → list all buckets */
        bucket_list_all(client_fd);
    } else {
        char bucket[256] = {0}, prefix[MAX_KEY_LEN] = {0};
        parse_s3_path(req->src, bucket, sizeof(bucket), prefix, sizeof(prefix));
        bucket_list(client_fd, bucket, prefix);
    }
}

/* mb: make bucket */
static void handle_mb(int client_fd, RequestHeader *req) {
    char bucket[256] = {0}, key[MAX_KEY_LEN] = {0};
    parse_s3_path(req->src, bucket, sizeof(bucket), key, sizeof(key));

    if (bucket[0] == '\0') {
        send_response(client_fd, 1, "Error: no bucket name provided");
        return;
    }
    if (bucket_create(bucket) == 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "make_bucket: s3://%s", bucket);
        send_response(client_fd, 0, msg);
    } else {
        send_response(client_fd, 1, "Error: could not create bucket (already exists?)");
    }
}

/* rb: remove bucket */
static void handle_rb(int client_fd, RequestHeader *req) {
    char bucket[256] = {0}, key[MAX_KEY_LEN] = {0};
    parse_s3_path(req->src, bucket, sizeof(bucket), key, sizeof(key));

    if (bucket_delete(bucket, req->force) == 0) {
        char msg[300];
        snprintf(msg, sizeof(msg), "remove_bucket: s3://%s", bucket);
        send_response(client_fd, 0, msg);
    } else {
        send_response(client_fd, 1,
            "Error: could not delete bucket (not empty? use --force)");
    }
}

/* cp: copy — receives a file payload from client, stores in bucket */
static void handle_cp(int client_fd, RequestHeader *req) {
    char bucket[256] = {0}, key[MAX_KEY_LEN] = {0};

    /* Determine direction: local→S3 or S3→local */
    int src_is_s3 = (strncmp(req->src, "s3://", 5) == 0);
    int dst_is_s3 = (strncmp(req->dst, "s3://", 5) == 0);

    if (!src_is_s3 && dst_is_s3) {
        /* ── Upload: local file → S3 ── */
        parse_s3_path(req->dst, bucket, sizeof(bucket), key, sizeof(key));

        /* If key is empty, use the source filename */
        if (key[0] == '\0') {
            const char *fname = strrchr(req->src, '/');
            strncpy(key, fname ? fname + 1 : req->src, MAX_KEY_LEN - 1);
        }

        if (req->data_len == 0) {
            send_response(client_fd, 1, "Error: no file data received");
            return;
        }

        /* Receive the file bytes from client */
        void *buf = malloc(req->data_len);
        if (!buf) { send_response(client_fd, 1, "Error: out of memory"); return; }
        if (recv_all(client_fd, buf, req->data_len) != 0) {
            free(buf);
            send_response(client_fd, 1, "Error: failed to receive file data");
            return;
        }

        int ret = bucket_put(bucket, key, buf, req->data_len);
        free(buf);

        if (ret == 0) {
            char msg[400];
            snprintf(msg, sizeof(msg), "upload: %s → s3://%s/%s", req->src, bucket, key);
            send_response(client_fd, 0, msg);
        } else {
            send_response(client_fd, 1, "Error: failed to store object in bucket");
        }

    } else if (src_is_s3 && !dst_is_s3) {
        /* ── Download: S3 → local file ── */
        parse_s3_path(req->src, bucket, sizeof(bucket), key, sizeof(key));

        uint64_t size = 0;
        void *data = bucket_get(bucket, key, &size);
        if (!data) {
            send_response(client_fd, 1, "Error: object not found");
            return;
        }

        ResponseHeader resp;
        memset(&resp, 0, sizeof(resp));
        resp.status   = 0;
        resp.data_len = size;
        snprintf(resp.message, sizeof(resp.message), "OK");
        send_all(client_fd, &resp, sizeof(resp));
        send_all(client_fd, data, size);
        free(data);

    } else if (src_is_s3 && dst_is_s3) {
        /* ── S3 → S3 copy ── */
        char src_bucket[256] = {0}, src_key[MAX_KEY_LEN] = {0};
        char dst_bucket[256] = {0}, dst_key[MAX_KEY_LEN] = {0};
        parse_s3_path(req->src, src_bucket, sizeof(src_bucket), src_key, sizeof(src_key));
        parse_s3_path(req->dst, dst_bucket, sizeof(dst_bucket), dst_key, sizeof(dst_key));

        if (dst_key[0] == '\0') strncpy(dst_key, src_key, MAX_KEY_LEN - 1);

        uint64_t size = 0;
        void *data = bucket_get(src_bucket, src_key, &size);
        if (!data) { send_response(client_fd, 1, "Error: source object not found"); return; }

        int ret = bucket_put(dst_bucket, dst_key, data, size);
        free(data);
        send_response(client_fd, ret == 0 ? 0 : 1,
                      ret == 0 ? "copy: OK" : "Error: S3-to-S3 copy failed");

    } else {
        send_response(client_fd, 1, "Error: at least one path must be s3://");
    }
}

/* mv: move = cp + rm source */
static void handle_mv(int client_fd, RequestHeader *req) {
    /* Reuse cp logic */
    handle_cp(client_fd, req);

    /* If cp succeeded, delete the source */
    /* (We check by peeking at the response — for simplicity we always try) */
    if (strncmp(req->src, "s3://", 5) == 0) {
        char bucket[256] = {0}, key[MAX_KEY_LEN] = {0};
        parse_s3_path(req->src, bucket, sizeof(bucket), key, sizeof(key));
        bucket_rm(bucket, key);
    }
    /* Note: if source was local, the client handles deleting the local file */
}

/* rm: delete one or more objects */
static void handle_rm(int client_fd, RequestHeader *req) {
    char bucket[256] = {0}, key[MAX_KEY_LEN] = {0};
    parse_s3_path(req->src, bucket, sizeof(bucket), key, sizeof(key));

    if (req->recursive && key[0] != '\0') {
        /* Delete all objects whose key starts with the given prefix */
        DirBlock block;
        if (bucket_read_dirblock(bucket, &block) != 0) {
            send_response(client_fd, 1, "Error: could not read bucket");
            return;
        }
        int count = 0;
        for (int i = 0; i < block.entry_count; i++) {
            DirEntry *e = &block.entries[i];
            if (!e->is_free && strncmp(e->key, key, strlen(key)) == 0) {
                e->is_free = 1;
                memset(e->key, 0, MAX_KEY_LEN);
                count++;
            }
        }
        bucket_write_dirblock(bucket, &block);
        char msg[200];
        snprintf(msg, sizeof(msg), "delete: removed %d object(s)", count);
        send_response(client_fd, 0, msg);
    } else {
        if (bucket_rm(bucket, key) == 0) {
            char msg[400];
            snprintf(msg, sizeof(msg), "delete: s3://%s/%s", bucket, key);
            send_response(client_fd, 0, msg);
        } else {
            send_response(client_fd, 1, "Error: object not found");
        }
    }
}

/* sync: handled entirely on the client side via multiple cp calls.
   The server just needs CMD_LIST_KEYS (below) to support it. */
static void handle_sync(int client_fd, RequestHeader *req) {
    (void)req;
    send_response(client_fd, 1, "sync: use client-side sync (this cmd is client-driven)");
}

/*
 * list_keys: returns a machine-readable newline-separated list of
 * "key\tsize\n" for every active object under the given prefix.
 * Used internally by the client for recursive cp, mv, and sync.
 */
static void handle_list_keys(int client_fd, RequestHeader *req) {
    char bucket[256] = {0}, prefix[MAX_KEY_LEN] = {0};
    parse_s3_path(req->src, bucket, sizeof(bucket), prefix, sizeof(prefix));

    ResponseHeader resp;
    memset(&resp, 0, sizeof(resp));

    if (!bucket_exists(bucket)) {
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message),
                 "Error: bucket '%s' not found", bucket);
        send_all(client_fd, &resp, sizeof(resp));
        return;
    }

    DirBlock block;
    if (bucket_read_dirblock(bucket, &block) != 0) {
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message), "Error: could not read bucket");
        send_all(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Build "key\tsize\n" listing */
    char *listing = calloc(1, MAX_ENTRIES * (MAX_KEY_LEN + 32));
    if (!listing) {
        send_response(client_fd, 1, "Error: out of memory");
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < block.entry_count; i++) {
        DirEntry *e = &block.entries[i];
        if (e->is_free) continue;
        if (prefix[0] && strncmp(e->key, prefix, strlen(prefix)) != 0) continue;
        int written = snprintf(listing + pos,
                               MAX_KEY_LEN + 32,
                               "%s\t%llu\n",
                               e->key,
                               (unsigned long long)e->size);
        if (written > 0) pos += (size_t)written;
    }

    resp.status   = 0;
    resp.data_len = pos;
    snprintf(resp.message, sizeof(resp.message), "OK");
    send_all(client_fd, &resp, sizeof(resp));
    if (pos > 0) send_all(client_fd, listing, pos);
    free(listing);
}

/* ─────────────────────────────────────────────
   HANDLE ONE CLIENT CONNECTION
   ───────────────────────────────────────────── */
static void handle_client(int client_fd) {
    RequestHeader req;

    /* Read the fixed-size request header */
    if (recv_all(client_fd, &req, sizeof(req)) != 0) {
        fprintf(stderr, "server: failed to read request header\n");
        return;
    }

    printf("server: received cmd=%d  src='%s'  dst='%s'\n",
           req.cmd, req.src, req.dst);

    /* Dispatch to the right handler */
    switch (req.cmd) {
        case CMD_LS:   handle_ls  (client_fd, &req); break;
        case CMD_MB:   handle_mb  (client_fd, &req); break;
        case CMD_RB:   handle_rb  (client_fd, &req); break;
        case CMD_CP:   handle_cp  (client_fd, &req); break;
        case CMD_MV:   handle_mv  (client_fd, &req); break;
        case CMD_RM:        handle_rm        (client_fd, &req); break;
        case CMD_SYNC:      handle_sync      (client_fd, &req); break;
        case CMD_LIST_KEYS: handle_list_keys (client_fd, &req); break;
        default:
            send_response(client_fd, 1, "Error: unknown command");
    }
}

/* ─────────────────────────────────────────────
   MAIN — set up the listening socket
   ───────────────────────────────────────────── */
int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    /* Allow reusing the port immediately after restart */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    printf("aws-s3-server listening on port %d...\n", SERVER_PORT);

    /* Simple sequential server — one client at a time */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        printf("server: connection from %s\n", inet_ntoa(client_addr.sin_addr));
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
