/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host check of Wine's in-process server (patch 0110). Linked with the
 * server objects built with WINE_INPROCESS_SERVER by
 * tools/test_wine_inprocess_server.sh, it starts the server inside this
 * process and completes the first step of Wine's client protocol (the
 * protocol version and the thread's request pipe, sent with SCM_RIGHTS)
 * over two connections, dropping the first one early: the server must
 * forget it without signalling its host, and keep accepting clients.
 * Usage: wine_inprocess_server_check NLS_DIR PROTOCOL_VERSION */
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern int pw_wineserver_connect(const char *nls_dir);

/* wine_server_receive_fd: one 32-bit handle with an fd attached. */
static int receive_fd(int socket_fd, uint32_t *handle)
{
    char control[256];
    struct iovec vec = { handle, sizeof(*handle) };
    struct msghdr msg;
    int fd = -1;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    if (recvmsg(socket_fd, &msg, 0) != (ssize_t)sizeof(*handle)) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    return fd;
}

static void handshake(const char *nls_dir, uint32_t version, int *client, int *request)
{
    uint32_t handle = 0;
    struct stat st;

    *client = pw_wineserver_connect(nls_dir);
    assert(*client >= 0);
    *request = receive_fd(*client, &handle);
    assert(*request >= 0 && handle == version);
    /* the thread's request pipe: this side writes requests */
    assert(!fstat(*request, &st) && S_ISFIFO(st.st_mode));
}

int main(int argc, char **argv)
{
    int first, first_request, second, second_request;
    uint32_t version;
    const struct timespec settle = { 0, 300 * 1000 * 1000 };

    if (argc != 3) return 2;
    version = (uint32_t)strtoul(argv[2], NULL, 10);

    handshake(argv[1], version, &first, &first_request);
    /* a client that leaves before initialising its first thread */
    close(first_request);
    close(first);
    nanosleep(&settle, NULL);

    handshake(argv[1], version, &second, &second_request);
    close(second_request);
    close(second);
    nanosleep(&settle, NULL);

    printf("in-process wineserver passed: two clients handshaked (protocol %u), host process intact\n",
           version);
    return 0;
}
