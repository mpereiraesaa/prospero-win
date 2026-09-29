/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* libc functions Winsock's Unix side (ws2_32.prx) names but a title does
 * not get: the old resolver's reverse lookup and its error code. Everything
 * else ws2_32 needs (sockets, getaddrinfo, getnameinfo) a title has.
 * gethostbyaddr answers through getnameinfo; h_errno is per thread, as the
 * FreeBSD libc's is. */
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

static _Thread_local int pw_h_errno;

int *__h_errno(void)
{
    return &pw_h_errno;
}

struct hostent *gethostbyaddr(const void *address, socklen_t length, int family)
{
    static _Thread_local struct hostent host;
    static _Thread_local char name[NI_MAXHOST];
    static _Thread_local char addresses[1][sizeof(struct in6_addr)];
    static _Thread_local char *address_list[2];
    static _Thread_local char *aliases[1];
    struct sockaddr_storage storage;
    socklen_t size;

    memset(&storage, 0, sizeof(storage));
    if (family == AF_INET && length == sizeof(struct in_addr)) {
        struct sockaddr_in *in = (struct sockaddr_in *)&storage;
        in->sin_family = AF_INET;
        in->sin_len = sizeof(*in);
        memcpy(&in->sin_addr, address, length);
        size = sizeof(*in);
    } else if (family == AF_INET6 && length == sizeof(struct in6_addr)) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&storage;
        in6->sin6_family = AF_INET6;
        in6->sin6_len = sizeof(*in6);
        memcpy(&in6->sin6_addr, address, length);
        size = sizeof(*in6);
    } else {
        pw_h_errno = NO_RECOVERY;
        return NULL;
    }
    if (getnameinfo((struct sockaddr *)&storage, size, name, sizeof(name), NULL, 0, NI_NAMEREQD)) {
        pw_h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    memcpy(addresses[0], address, length);
    address_list[0] = addresses[0];
    address_list[1] = NULL;
    aliases[0] = NULL;
    host.h_name = name;
    host.h_aliases = aliases;
    host.h_addrtype = family;
    host.h_length = (int)length;
    host.h_addr_list = address_list;
    return &host;
}
