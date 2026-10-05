/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "ps5log_ps5_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>

typedef struct ps5log_sce_epoll_event {
    uint32_t events;
    uint32_t pad;
    uint64_t ident;
    union {
        void *pointer;
        uint32_t value;
        uint64_t value64;
        int socket;
    } data;
} ps5log_sce_epoll_event;

_Static_assert(sizeof(ps5log_sce_epoll_event) == 24,
               "sceNet epoll event ABI mismatch");

enum {
    PS5LOG_SCE_EPOLLIN = 0x00000001,
    PS5LOG_SCE_EPOLLOUT = 0x00000002,
    PS5LOG_SCE_EPOLLERR = 0x00000008,
    PS5LOG_SCE_EPOLLHUP = 0x00000010,
    PS5LOG_SCE_EPOLL_CTL_ADD = 1,
    PS5LOG_SCE_SO_NBIO = 0x1200,
};

int sceNetConnect(int, const struct sockaddr *, socklen_t);
int sceNetEpollControl(int, int, int, ps5log_sce_epoll_event *);
int sceNetEpollCreate(const char *, int);
int sceNetEpollDestroy(int);
int sceNetEpollWait(int, ps5log_sce_epoll_event *, int, int);
int *sceNetErrnoLoc(void);
int sceNetGetsockopt(int, int, int, void *, socklen_t *);
int sceNetSend(int, const void *, size_t, int);
int sceNetSetsockopt(int, int, int, const void *, socklen_t);
int sceNetShutdown(int, int);
int sceNetSocket(const char *, int, int, int);
int sceNetSocketClose(int);

static int result(int value) {
    if (value < 0) {
        int *network_errno = sceNetErrnoLoc();
        if (network_errno) errno = *network_errno;
        return -1;
    }
    return value;
}

int ps5log_ps5_socket(int domain, int type, int protocol) {
    return result(sceNetSocket("ps5log", domain, type, protocol));
}

int ps5log_ps5_connect(int socket_id, const struct sockaddr *address,
                       socklen_t length) {
    return result(sceNetConnect(socket_id, address, length));
}

long ps5log_ps5_send(int socket_id, const void *buffer, size_t length,
                     int flags) {
    (void)flags;
    return result(sceNetSend(socket_id, buffer, length, 0));
}

int ps5log_ps5_setsockopt(int socket_id, int level, int option,
                          const void *value, socklen_t length) {
    return result(sceNetSetsockopt(socket_id, level, option, value, length));
}

int ps5log_ps5_getsockopt(int socket_id, int level, int option,
                          void *value, socklen_t *length) {
    return result(sceNetGetsockopt(socket_id, level, option, value, length));
}

int ps5log_ps5_shutdown(int socket_id, int how) {
    return result(sceNetShutdown(socket_id, how));
}

int ps5log_ps5_close(int socket_id) {
    return result(sceNetSocketClose(socket_id));
}

int ps5log_ps5_fcntl(int socket_id, int command, ...) {
    if (command == F_GETFL) {
        int enabled = 0;
        socklen_t length = sizeof(enabled);
        if (result(sceNetGetsockopt(socket_id, SOL_SOCKET,
                                   PS5LOG_SCE_SO_NBIO, &enabled,
                                   &length)) < 0) return -1;
        return enabled ? O_NONBLOCK : 0;
    }
    if (command == F_SETFL) {
        va_list arguments;
        int flags;
        int enabled;
        va_start(arguments, command);
        flags = va_arg(arguments, int);
        va_end(arguments);
        enabled = (flags & O_NONBLOCK) != 0;
        return result(sceNetSetsockopt(socket_id, SOL_SOCKET,
                                      PS5LOG_SCE_SO_NBIO, &enabled,
                                      sizeof(enabled)));
    }
    errno = EINVAL;
    return -1;
}

int ps5log_ps5_poll(struct pollfd *descriptors, unsigned long count,
                    int timeout_milliseconds) {
    ps5log_sce_epoll_event interest = {0};
    ps5log_sce_epoll_event ready = {0};
    int epoll;
    int wait_result;
    int timeout_microseconds;
    if (!descriptors || count != 1 || descriptors[0].fd < 0) {
        errno = EINVAL;
        return -1;
    }
    descriptors[0].revents = 0;
    if (descriptors[0].events & POLLIN) interest.events |= PS5LOG_SCE_EPOLLIN;
    if (descriptors[0].events & POLLOUT) interest.events |= PS5LOG_SCE_EPOLLOUT;
    interest.data.value = 0;
    epoll = result(sceNetEpollCreate("ps5log-poll", 0));
    if (epoll < 0) return -1;
    if (result(sceNetEpollControl(epoll, PS5LOG_SCE_EPOLL_CTL_ADD,
                                  descriptors[0].fd, &interest)) < 0) {
        (void)sceNetEpollDestroy(epoll);
        return -1;
    }
    timeout_microseconds = timeout_milliseconds < 0
        ? -1 : timeout_milliseconds * 1000;
    wait_result = result(sceNetEpollWait(epoll, &ready, 1,
                                        timeout_microseconds));
    if (wait_result > 0) {
        if (ready.events & PS5LOG_SCE_EPOLLIN) descriptors[0].revents |= POLLIN;
        if (ready.events & PS5LOG_SCE_EPOLLOUT) descriptors[0].revents |= POLLOUT;
        if (ready.events & PS5LOG_SCE_EPOLLERR) descriptors[0].revents |= POLLERR;
        if (ready.events & PS5LOG_SCE_EPOLLHUP) descriptors[0].revents |= POLLHUP;
    }
    (void)sceNetEpollDestroy(epoll);
    return wait_result;
}
