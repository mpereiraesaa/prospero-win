/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The resolver behind ws2_32.prx's getaddrinfo (pw_ws2_32_libc.c): one answer
 * is an address family and the address's bytes. On the console the console's
 * own resolver (libSceNet) answers; the host test installs a backend. */
#ifndef PW_WS2_32_RESOLVER_H
#define PW_WS2_32_RESOLVER_H

struct pw_ws2_address {
    int family;                 /* AF_INET or AF_INET6 */
    unsigned char bytes[16];    /* 4 or 16 of them */
};

/* Resolve NAME for FAMILY (AF_UNSPEC, AF_INET or AF_INET6) into up to two
 * answers, IPv4 first; returns 0, or an EAI_ error. The host test sets it;
 * on the console it is the libSceNet backend. */
extern int (*pw_ws2_32_resolve)(const char *name, int family, struct pw_ws2_address answers[2], int *count);

/* A name the backend did not know is not asked again for this long. */
#define PW_WS2_32_NEGATIVE_SECONDS 5

/* The clock the negative cache runs on, in seconds, monotonic; the host
 * test installs its own to move time. */
extern long long (*pw_ws2_32_now)(void);

#endif
