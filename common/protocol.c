#include "protocol.h"

#include <sys/socket.h>
#include <stdint.h>
#include <stddef.h>

/*
 * send_all — keep sending until every byte is delivered.
 *
 * Returns 0 on success, -1 on error.
 */
int send_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, 0);
        if (sent <= 0) return -1;   /* connection closed or error */
        ptr       += sent;
        remaining -= sent;
    }
    return 0;
}

/*
 * recv_all — keep reading until every byte is received.
 *
 * Returns 0 on success, -1 on error or connection closed.
 */
int recv_all(int fd, void *buf, size_t len) {
    char  *ptr = (char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t got = recv(fd, ptr, remaining, 0);
        if (got <= 0) return -1;    /* connection closed or error */
        ptr       += got;
        remaining -= got;
    }
    return 0;
}
