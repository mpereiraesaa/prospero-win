/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PS5LOG_PS5_NET_H
#define PS5LOG_PS5_NET_H

#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int ps5log_ps5_socket(int domain, int type, int protocol);
int ps5log_ps5_connect(int socket_id, const struct sockaddr *address,
                       socklen_t length);
long ps5log_ps5_send(int socket_id, const void *buffer, size_t length,
                     int flags);
int ps5log_ps5_setsockopt(int socket_id, int level, int option,
                          const void *value, socklen_t length);
int ps5log_ps5_getsockopt(int socket_id, int level, int option,
                          void *value, socklen_t *length);
int ps5log_ps5_shutdown(int socket_id, int how);
int ps5log_ps5_close(int socket_id);
int ps5log_ps5_fcntl(int socket_id, int command, ...);
int ps5log_ps5_poll(struct pollfd *descriptors, unsigned long count,
                    int timeout_milliseconds);

#ifdef __cplusplus
}
#endif

#define PS5LOG_NET_SOCKET ps5log_ps5_socket
#define PS5LOG_NET_CONNECT ps5log_ps5_connect
#define PS5LOG_NET_SEND ps5log_ps5_send
#define PS5LOG_NET_SETSOCKOPT ps5log_ps5_setsockopt
#define PS5LOG_NET_GETSOCKOPT ps5log_ps5_getsockopt
#define PS5LOG_NET_SHUTDOWN ps5log_ps5_shutdown
#define PS5LOG_NET_CLOSE ps5log_ps5_close
#define PS5LOG_NET_FCNTL ps5log_ps5_fcntl
#define PS5LOG_NET_POLL ps5log_ps5_poll

#endif
