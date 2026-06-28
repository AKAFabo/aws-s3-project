#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../common/protocol.h"

/* ─────────────────────────────────────────────
   CONNECT TO SERVER
   Returns a connected socket fd, or -1 on failure.
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
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* ─────────────────────────────────────────────
   RECEIVE AND PRINT THE SERVER RESPONSE
   Also handles writing a file payload to disk
   when the server sends data (e.g. download).
   ───────────────────────────────────────────── */
static int receive_response(int fd, const char *save_path) {
    ResponseHeader resp;
    if (recv_all(fd, &resp, sizeof(resp)) != 0) {
        fprintf(stderr, "client: failed to read server response\n");
        return -1;
    }

    printf("%s\n", resp.message);

    if (resp.data_len > 0) {
        if (save_path && save_path[0] != '\0') {
            /* Save payload to a local file (download) */
            void *buf = malloc(resp.data_len);
            if (!buf) { fprintf(stderr, "out of memory\n"); return -1; }
            if (recv_all(fd, buf, resp.data_len) != 0) {
                free(buf); return -1;
            }
            FILE *f = fopen(save_path, "wb");
            if (!f) { perror("fopen"); free(buf); return -1; }
            fwrite(buf, 1, resp.data_len, f);
            fclose(f);
            free(buf);
            printf("saved to: %s\n", save_path);
        } else {
            /* Print text payload (listing output) */
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
   SEND A FILE to the server as part of a cp/mv upload
   ───────────────────────────────────────────── */
static int send_file(int fd, RequestHeader *req, const char *local_path) {
    /* Open and stat the local file */
    struct stat st;
    if (stat(local_path, &st) != 0) {
        perror(local_path);
        return -1;
    }
    req->data_len = (uint64_t)st.st_size;

    /* Send the header first */
    if (send_all(fd, req, sizeof(*req)) != 0) return -1;

    /* Then stream the file in chunks */
    FILE *f = fopen(local_path, "rb");
    if (!f) { perror(local_path); return -1; }

    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (send_all(fd, chunk, n) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* ─────────────────────────────────────────────
   USAGE / HELP
   ───────────────────────────────────────────── */
static void print_usage(void) {
    printf("Usage:\n");
    printf("  aws-s3 ls [s3://bucket/prefix]\n");
    printf("  aws-s3 mb s3://bucket\n");
    printf("  aws-s3 rb s3://bucket [--force]\n");
    printf("  aws-s3 cp <src> <dst> [--recursive]\n");
    printf("  aws-s3 mv <src> <dst> [--recursive]\n");
    printf("  aws-s3 rm s3://bucket/key [--recursive]\n");
    printf("  aws-s3 sync <src> <dst> [--delete]\n");
}

/* ─────────────────────────────────────────────
   MAIN
   ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(); return 1; }

    const char *subcmd = argv[1];

    /* Build the request header */
    RequestHeader req;
    memset(&req, 0, sizeof(req));

    /* Scan for flags anywhere in the arguments */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) req.recursive   = 1;
        if (strcmp(argv[i], "--force")     == 0) req.force       = 1;
        if (strcmp(argv[i], "--delete")    == 0) req.delete_flag = 1;
    }

    /* ── Parse each subcommand ── */

    if (strcmp(subcmd, "ls") == 0) {
        req.cmd = CMD_LS;
        if (argc >= 3) strncpy(req.src, argv[2], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    } else if (strcmp(subcmd, "mb") == 0) {
        if (argc < 3) { fprintf(stderr, "mb: missing bucket path\n"); return 1; }
        req.cmd = CMD_MB;
        strncpy(req.src, argv[2], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    } else if (strcmp(subcmd, "rb") == 0) {
        if (argc < 3) { fprintf(stderr, "rb: missing bucket path\n"); return 1; }
        req.cmd = CMD_RB;
        strncpy(req.src, argv[2], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    } else if (strcmp(subcmd, "cp") == 0) {
        if (argc < 4) { fprintf(stderr, "cp: need src and dst\n"); return 1; }
        req.cmd = CMD_CP;
        strncpy(req.src, argv[2], MAX_KEY_LEN - 1);
        strncpy(req.dst, argv[3], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;

        int src_is_s3 = (strncmp(req.src, "s3://", 5) == 0);
        if (!src_is_s3) {
            /* Upload: send file data after the header */
            if (send_file(fd, &req, req.src) != 0) { close(fd); return 1; }
        } else {
            /* Download or S3-to-S3: no payload from client */
            send_all(fd, &req, sizeof(req));
        }

        /* For downloads, save to dst path */
        const char *save = (strncmp(req.dst, "s3://", 5) != 0) ? req.dst : NULL;
        receive_response(fd, save);
        close(fd);

    } else if (strcmp(subcmd, "mv") == 0) {
        if (argc < 4) { fprintf(stderr, "mv: need src and dst\n"); return 1; }
        req.cmd = CMD_MV;
        strncpy(req.src, argv[2], MAX_KEY_LEN - 1);
        strncpy(req.dst, argv[3], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;

        int src_is_s3 = (strncmp(req.src, "s3://", 5) == 0);
        if (!src_is_s3) {
            if (send_file(fd, &req, req.src) != 0) { close(fd); return 1; }
            receive_response(fd, NULL);
            /* Delete local source file after successful upload */
            remove(req.src);
        } else {
            send_all(fd, &req, sizeof(req));
            const char *save = (strncmp(req.dst, "s3://", 5) != 0) ? req.dst : NULL;
            receive_response(fd, save);
        }
        close(fd);

    } else if (strcmp(subcmd, "rm") == 0) {
        if (argc < 3) { fprintf(stderr, "rm: missing path\n"); return 1; }
        req.cmd = CMD_RM;
        strncpy(req.src, argv[2], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    } else if (strcmp(subcmd, "sync") == 0) {
        if (argc < 4) { fprintf(stderr, "sync: need src and dst\n"); return 1; }
        req.cmd = CMD_SYNC;
        strncpy(req.src, argv[2], MAX_KEY_LEN - 1);
        strncpy(req.dst, argv[3], MAX_KEY_LEN - 1);

        int fd = connect_to_server();
        if (fd < 0) return 1;
        send_all(fd, &req, sizeof(req));
        receive_response(fd, NULL);
        close(fd);

    } else {
        fprintf(stderr, "Unknown command: %s\n", subcmd);
        print_usage();
        return 1;
    }

    return 0;
}
