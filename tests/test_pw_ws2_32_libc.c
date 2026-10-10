/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Built with the resolver's names prefixed (Makefile), so the calls below
 * reach wine/ps5/pw_ws2_32_libc.c and not glibc's resolver. */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

int *__h_errno(void);
#include "../wine/ps5/pw_ws2_32_resolver.h"

static int count(const struct addrinfo *info)
{
    int n = 0;
    for (; info; info = info->ai_next)
        n++;
    return n;
}

static const char *text(const struct addrinfo *info)
{
    static char buffer[INET6_ADDRSTRLEN];
    const void *address = info->ai_family == AF_INET
        ? (const void *)&((const struct sockaddr_in *)info->ai_addr)->sin_addr
        : (const void *)&((const struct sockaddr_in6 *)info->ai_addr)->sin6_addr;
    assert(inet_ntop(info->ai_family, address, buffer, sizeof(buffer)));
    return buffer;
}

static unsigned port(const struct addrinfo *info)
{
    return info->ai_family == AF_INET ? ntohs(((const struct sockaddr_in *)info->ai_addr)->sin_port)
                                      : ntohs(((const struct sockaddr_in6 *)info->ai_addr)->sin6_port);
}

static struct addrinfo hints_of(int family, int socktype, int protocol, int flags)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    hints.ai_flags = flags;
    return hints;
}

/* A resolver backend as the console's: example.com has one address per
 * family; anything else is unknown. Counts its calls, so the names the
 * stub answers itself are seen to stay away from it. */
static int backend_calls;
static long long fake_now = 1000;
static long long fake_clock(void) { return fake_now; }
static int fake_resolve(const char *name, int family, struct pw_ws2_address answers[2], int *found)
{
    backend_calls++;
    *found = 0;
    if (strcmp(name, "example.com"))
        return EAI_NONAME;
    if (family != AF_INET6) {
        answers[*found].family = AF_INET;
        memcpy(answers[*found].bytes, "\x5d\xb8\xd8\x22", 4);     /* 93.184.216.34 */
        (*found)++;
    }
    if (family != AF_INET) {
        static const unsigned char six[16] = { 0x26, 0x06, 0x28, 0x00, [15] = 0x46 };
        answers[*found].family = AF_INET6;
        memcpy(answers[*found].bytes, six, 16);
        (*found)++;
    }
    return 0;
}

static void test_resolver_backend(void)
{
    struct addrinfo hints = {0}, *info;
    struct hostent *host;

    pw_ws2_32_resolve = fake_resolve;
    pw_ws2_32_now = fake_clock;
    backend_calls = 0;
    /* A real name: the backend's answers, IPv4 first, TCP and UDP each. */
    assert(!getaddrinfo("example.com", "443", NULL, &info));
    assert(count(info) == 4 && info->ai_family == AF_INET && !strcmp(text(info), "93.184.216.34"));
    assert(info->ai_socktype == SOCK_STREAM && ntohs(((struct sockaddr_in *)info->ai_addr)->sin_port) == 443);
    assert(info->ai_next->ai_next->ai_family == AF_INET6);
    freeaddrinfo(info);
    assert(backend_calls == 1);
    /* The family asked for is passed on, and the canonical name is the name. */
    hints.ai_family = AF_INET6; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_CANONNAME;
    assert(!getaddrinfo("example.com", NULL, &hints, &info));
    assert(count(info) == 1 && info->ai_family == AF_INET6 && !strcmp(info->ai_canonname, "example.com"));
    freeaddrinfo(info);
    /* gethostbyname goes through it too, IPv4 only. */
    host = gethostbyname("example.com");
    assert(host && host->h_addrtype == AF_INET && !strcmp(host->h_name, "example.com") &&
           !memcmp(host->h_addr_list[0], "\x5d\xb8\xd8\x22", 4));
    /* What the backend does not know stays unknown; what needs no backend
     * never reaches it: numeric addresses, AI_NUMERICHOST and localhost. */
    assert(backend_calls == 3);
    assert(getaddrinfo("nowhere.invalid", "80", NULL, &info) == EAI_NONAME && !info && backend_calls == 4);
    memset(&hints, 0, sizeof(hints)); hints.ai_flags = AI_NUMERICHOST;
    assert(getaddrinfo("example.com", "80", &hints, &info) == EAI_NONAME && !info);
    assert(!getaddrinfo("127.0.0.1", "80", NULL, &info)); freeaddrinfo(info);
    assert(!getaddrinfo("localhost", "80", NULL, &info)); freeaddrinfo(info);
    assert(backend_calls == 4);
    /* A single-label name never reaches the backend: not found at once,
     * through getaddrinfo and gethostbyname alike (the PC's computer name
     * in a prefix, which GTA IV asks for tens of thousands of times a
     * minute), while a dotted one does. */
    assert(getaddrinfo("DESKTOP-PC", "80", NULL, &info) == EAI_NONAME && !info);
    assert(!gethostbyname("desktop-pc") && *__h_errno() == HOST_NOT_FOUND);
    assert(getaddrinfo("desktop-pc", NULL, NULL, &info) == EAI_NONAME && !info);
    assert(backend_calls == 4);
    assert(getaddrinfo("desktop-pc.lan", "80", NULL, &info) == EAI_NONAME && !info && backend_calls == 5);
    /* The local name gethostname reports, in any case, is loopback here and
     * never a question for the backend: a game's probe of its own host
     * name stays local and instant. */
    assert(!getaddrinfo("PS5", "80", NULL, &info) && !strcmp(text(info), "127.0.0.1")); freeaddrinfo(info);
    assert(!getaddrinfo("ps5", NULL, NULL, &info) && !strcmp(text(info), "127.0.0.1")); freeaddrinfo(info);
    assert(gethostbyname("Ps5") && !memcmp(gethostbyname("Ps5")->h_addr_list[0], "\x7f\0\0\x01", 4));
    assert(backend_calls == 5);
    pw_ws2_32_resolve = NULL;
}

/* A name the backend did not know is answered from the negative cache for
 * PW_WS2_32_NEGATIVE_SECONDS, per family asked, then asked again; the
 * table holds 16 names and replaces the oldest. */
static void test_negative_cache(void)
{
    struct addrinfo hints = {0}, *info;
    char name[32];
    int i;

    pw_ws2_32_resolve = fake_resolve;
    pw_ws2_32_now = fake_clock;
    fake_now = 5000;
    backend_calls = 0;
    assert(getaddrinfo("gone.invalid", "80", NULL, &info) == EAI_NONAME && backend_calls == 1);
    assert(getaddrinfo("gone.invalid", "80", NULL, &info) == EAI_NONAME && backend_calls == 1);
    assert(getaddrinfo("GONE.invalid", "443", NULL, &info) == EAI_NONAME && backend_calls == 1);
    assert(!gethostbyname("gone.invalid") && *__h_errno() == HOST_NOT_FOUND && backend_calls == 2);  /* AF_INET: its own entry */
    assert(!gethostbyname("gone.invalid") && backend_calls == 2);
    hints.ai_family = AF_INET6;
    assert(getaddrinfo("gone.invalid", "80", &hints, &info) == EAI_NONAME && backend_calls == 3);
    fake_now += PW_WS2_32_NEGATIVE_SECONDS - 1;
    assert(getaddrinfo("gone.invalid", "80", NULL, &info) == EAI_NONAME && backend_calls == 3);
    fake_now += 1;
    assert(getaddrinfo("gone.invalid", "80", NULL, &info) == EAI_NONAME && backend_calls == 4);
    /* A known name is not cached as unknown, and the window is per entry. */
    assert(!getaddrinfo("example.com", "80", NULL, &info) && backend_calls == 5); freeaddrinfo(info);
    assert(!getaddrinfo("example.com", "80", NULL, &info) && backend_calls == 6); freeaddrinfo(info);
    /* Sixteen fresh failures push the oldest entry out. */
    backend_calls = 0;
    for (i = 0; i < 16; i++) {
        snprintf(name, sizeof(name), "host%d.invalid", i);
        assert(getaddrinfo(name, "80", NULL, &info) == EAI_NONAME);
    }
    assert(backend_calls == 16);
    assert(getaddrinfo("gone.invalid", "80", NULL, &info) == EAI_NONAME && backend_calls == 17);
    assert(getaddrinfo("host15.invalid", "80", NULL, &info) == EAI_NONAME && backend_calls == 17);
    pw_ws2_32_resolve = NULL;
}

static void test_numeric(void)
{
    struct addrinfo hints = hints_of(AF_UNSPEC, SOCK_STREAM, 0, 0), *info;

    assert(!getaddrinfo("11.22.33.44", "8080", &hints, &info));
    assert(count(info) == 1 && info->ai_family == AF_INET && !strcmp(text(info), "11.22.33.44"));
    assert(info->ai_socktype == SOCK_STREAM && info->ai_protocol == IPPROTO_TCP);
    assert(info->ai_addrlen == sizeof(struct sockaddr_in) && port(info) == 8080 && !info->ai_canonname);
    freeaddrinfo(info);

    assert(!getaddrinfo("fe80::1:2", "0", &hints, &info));
    assert(count(info) == 1 && info->ai_family == AF_INET6 && !strcmp(text(info), "fe80::1:2"));
    assert(info->ai_addrlen == sizeof(struct sockaddr_in6) && port(info) == 0);
    freeaddrinfo(info);

    /* A numeric address of the other family is not converted. */
    hints = hints_of(AF_INET6, 0, 0, 0);
    assert(getaddrinfo("44.55.66.77", NULL, &hints, &info) == EAI_NONAME && !info);
    hints = hints_of(AF_INET, 0, 0, 0);
    assert(getaddrinfo("::1", NULL, &hints, &info) == EAI_NONAME && !info);

    /* No hints: TCP and UDP for the one address, canonical name only on request. */
    assert(!getaddrinfo("127.0.0.2", "53", NULL, &info));
    assert(count(info) == 2 && info->ai_socktype == SOCK_STREAM && info->ai_protocol == IPPROTO_TCP);
    assert(info->ai_next->ai_socktype == SOCK_DGRAM && info->ai_next->ai_protocol == IPPROTO_UDP);
    assert(port(info) == 53 && port(info->ai_next) == 53 && !strcmp(text(info->ai_next), "127.0.0.2"));
    freeaddrinfo(info);

    hints = hints_of(AF_INET, 0, 0, AI_CANONNAME | AI_NUMERICHOST);
    assert(!getaddrinfo("1.2.3.4", NULL, &hints, &info));
    assert(info->ai_canonname && !strcmp(info->ai_canonname, "1.2.3.4"));
    assert(!info->ai_next->ai_canonname && info->ai_flags == (AI_CANONNAME | AI_NUMERICHOST));
    freeaddrinfo(info);
}

static void test_null_host(void)
{
    struct addrinfo hints = hints_of(AF_UNSPEC, SOCK_DGRAM, 0, AI_PASSIVE), *info;

    /* Wildcard addresses to bind to, IPv4 first. */
    assert(!getaddrinfo(NULL, "27015", &hints, &info));
    assert(count(info) == 2);
    assert(info->ai_family == AF_INET && port(info) == 27015);
    assert(((struct sockaddr_in *)info->ai_addr)->sin_addr.s_addr == htonl(INADDR_ANY));
    assert(info->ai_next->ai_family == AF_INET6 && !strcmp(text(info->ai_next), "::"));
    assert(info->ai_protocol == IPPROTO_UDP && info->ai_next->ai_socktype == SOCK_DGRAM);
    freeaddrinfo(info);

    /* Without AI_PASSIVE, loopback to connect to. */
    hints = hints_of(AF_INET, SOCK_STREAM, 0, 0);
    assert(!getaddrinfo(NULL, "80", &hints, &info));
    assert(count(info) == 1 && !strcmp(text(info), "127.0.0.1") && port(info) == 80);
    freeaddrinfo(info);
    hints = hints_of(AF_INET6, SOCK_STREAM, 0, 0);
    assert(!getaddrinfo(NULL, "80", &hints, &info));
    assert(count(info) == 1 && !strcmp(text(info), "::1"));
    freeaddrinfo(info);

    assert(getaddrinfo(NULL, NULL, NULL, &info) == EAI_NONAME && !info);
    hints = hints_of(AF_UNSPEC, 0, 0, AI_CANONNAME);
    assert(getaddrinfo(NULL, "80", &hints, &info) == EAI_BADFLAGS && !info);
}

static void test_local_names(void)
{
    struct addrinfo hints = hints_of(AF_UNSPEC, SOCK_STREAM, 0, AI_CANONNAME | AI_PASSIVE), *info;
    char own[256];

    /* localhost is loopback, even with AI_PASSIVE, and keeps its name. */
    assert(!getaddrinfo("LocalHost", NULL, &hints, &info));
    assert(count(info) == 2 && !strcmp(text(info), "127.0.0.1") && !strcmp(text(info->ai_next), "::1"));
    assert(!strcmp(info->ai_canonname, "LocalHost") && !info->ai_next->ai_canonname);
    freeaddrinfo(info);

    assert(!gethostname(own, sizeof(own)) && !strcmp(own, "PS5"));
    /* A too-small result must fail without writing past the supplied size. */
    for (size_t n = 0; n < 4; ++n) {
        char small[5] = "xxxx";
        errno = 0;
        assert(gethostname(small, n) == -1 && errno == ENAMETOOLONG);
        assert(!memcmp(small, "xxxx", sizeof(small)));
    }
    char exact[5] = {'x', 'x', 'x', 'x', '!'};
    assert(!gethostname(exact, 4) && !memcmp(exact, "PS5\0", 4) && exact[4] == '!');
    hints = hints_of(AF_INET, SOCK_STREAM, 0, 0);
    assert(!getaddrinfo(own, "1", &hints, &info));
    assert(count(info) == 1 && !strcmp(text(info), "127.0.0.1"));
    freeaddrinfo(info);

    assert(!getaddrinfo("pS5", NULL, &hints, &info));
    assert(count(info) == 1 && !strcmp(text(info), "127.0.0.1"));
    freeaddrinfo(info);

    /* GTA IV's local-address probe: hostname, AF_INET, no service or type. */
    hints = hints_of(AF_INET, 0, 0, 0);
    assert(!getaddrinfo(own, NULL, &hints, &info));
    assert(count(info) == 2 && !strcmp(text(info), "127.0.0.1"));
    freeaddrinfo(info);

    /* Other single-label names are not this machine: a prefix made on a PC
     * asks for the PC's computer name, and the game must see that fail. */
    assert(getaddrinfo("some-pc", "1", &hints, &info) == EAI_NONAME && !info);

    /* Names are not resolved by number only, or at all otherwise. */
    hints = hints_of(AF_UNSPEC, 0, 0, AI_NUMERICHOST);
    assert(getaddrinfo("localhost", NULL, &hints, &info) == EAI_NONAME && !info);
    assert(getaddrinfo("example.com", "80", NULL, &info) == EAI_NONAME && !info);
    assert(getaddrinfo("", "80", NULL, &info) == EAI_NONAME && !info);
    assert(getaddrinfo("localhost.example", "80", NULL, &info) == EAI_NONAME);
}

static void test_hints(void)
{
    struct addrinfo hints = hints_of(AF_UNIX, 0, 0, 0), *info;

    assert(getaddrinfo("127.0.0.1", NULL, &hints, &info) == EAI_FAMILY && !info);
    hints = hints_of(AF_INET, SOCK_SEQPACKET, 0, 0);
    assert(getaddrinfo("127.0.0.1", NULL, &hints, &info) == EAI_SOCKTYPE && !info);

    /* A protocol alone picks its socket type; any other is raw. */
    hints = hints_of(AF_INET, 0, IPPROTO_UDP, 0);
    assert(!getaddrinfo("127.0.0.1", NULL, &hints, &info));
    assert(count(info) == 1 && info->ai_socktype == SOCK_DGRAM && info->ai_protocol == IPPROTO_UDP);
    freeaddrinfo(info);
    hints = hints_of(AF_INET, 0, IPPROTO_ICMP, 0);
    assert(!getaddrinfo("127.0.0.1", NULL, &hints, &info));
    assert(count(info) == 1 && info->ai_socktype == SOCK_RAW && info->ai_protocol == IPPROTO_ICMP);
    freeaddrinfo(info);
    hints = hints_of(AF_INET, SOCK_RAW, 0, 0);
    assert(!getaddrinfo("127.0.0.1", NULL, &hints, &info));
    assert(count(info) == 1 && info->ai_socktype == SOCK_RAW && info->ai_protocol == 0);
    freeaddrinfo(info);
}

static void test_ports(void)
{
    struct addrinfo hints = hints_of(AF_INET, SOCK_STREAM, 0, 0), *info;

    assert(!getaddrinfo("127.0.0.1", "65535", &hints, &info) && port(info) == 65535);
    freeaddrinfo(info);
    assert(!getaddrinfo("127.0.0.1", NULL, &hints, &info) && port(info) == 0);
    freeaddrinfo(info);
    /* No services database: a name, a sign or a number out of range fails. */
    assert(getaddrinfo("127.0.0.1", "65536", &hints, &info) == EAI_SERVICE && !info);
    assert(getaddrinfo("127.0.0.1", "http", &hints, &info) == EAI_SERVICE && !info);
    assert(getaddrinfo("127.0.0.1", "-1", &hints, &info) == EAI_SERVICE && !info);
    assert(getaddrinfo("127.0.0.1", "80x", &hints, &info) == EAI_SERVICE && !info);
    assert(getaddrinfo("127.0.0.1", "", &hints, &info) == EAI_SERVICE && !info);
    hints.ai_flags = AI_NUMERICSERV;
    assert(getaddrinfo("127.0.0.1", "http", &hints, &info) == EAI_NONAME && !info);
}

static void test_free_chain(void)
{
    struct addrinfo *info;

    /* Four entries (two addresses, two socket types), each freed on its own. */
    assert(!getaddrinfo("localhost", "7", NULL, &info));
    assert(count(info) == 4);
    assert(info->ai_family == AF_INET && info->ai_next->ai_family == AF_INET);
    assert(info->ai_next->ai_next->ai_family == AF_INET6);
    assert(info->ai_next->ai_next->ai_next->ai_family == AF_INET6);
    freeaddrinfo(info);
    freeaddrinfo(NULL);
}

static void test_getnameinfo(void)
{
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
    char host[NI_MAXHOST], service[NI_MAXSERV], small[4];

    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(443);
    assert(inet_pton(AF_INET, "66.77.88.99", &in.sin_addr) == 1);
    assert(!getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), service, sizeof(service), 0));
    assert(!strcmp(host, "66.77.88.99") && !strcmp(service, "443"));
    assert(getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), NULL, 0, NI_NAMEREQD)
           == EAI_NONAME);
    assert(getnameinfo((struct sockaddr *)&in, sizeof(in), small, sizeof(small), NULL, 0, 0) == EAI_OVERFLOW);
    assert(getnameinfo((struct sockaddr *)&in, sizeof(in), NULL, 0, small, 3, 0) == EAI_OVERFLOW);
    assert(getnameinfo((struct sockaddr *)&in, sizeof(in), NULL, 0, NULL, 0, 0) == EAI_NONAME);
    assert(getnameinfo((struct sockaddr *)&in, sizeof(in) - 1, host, sizeof(host), NULL, 0, 0) == EAI_FAMILY);

    /* Loopback is localhost unless a number is asked for. */
    assert(inet_pton(AF_INET, "127.0.0.1", &in.sin_addr) == 1);
    assert(!getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), NULL, 0, NI_NAMEREQD));
    assert(!strcmp(host, "localhost"));
    assert(!getnameinfo((struct sockaddr *)&in, sizeof(in), host, sizeof(host), NULL, 0, NI_NUMERICHOST));
    assert(!strcmp(host, "127.0.0.1"));

    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(1);
    assert(inet_pton(AF_INET6, "::1", &in6.sin6_addr) == 1);
    assert(!getnameinfo((struct sockaddr *)&in6, sizeof(in6), host, sizeof(host), service, sizeof(service), 0));
    assert(!strcmp(host, "localhost") && !strcmp(service, "1"));
    assert(inet_pton(AF_INET6, "2001:db8::5", &in6.sin6_addr) == 1);
    assert(!getnameinfo((struct sockaddr *)&in6, sizeof(in6), host, sizeof(host), NULL, 0, 0));
    assert(!strcmp(host, "2001:db8::5"));

    struct sockaddr other = { .sa_family = AF_UNIX };
    assert(getnameinfo(&other, sizeof(other), host, sizeof(host), NULL, 0, 0) == EAI_FAMILY);
    assert(getnameinfo(NULL, 0, host, sizeof(host), NULL, 0, 0) == EAI_FAMILY);
}

static void test_hostent(void)
{
    struct hostent *host;
    struct in_addr address;

    host = gethostbyname("localhost");
    assert(host && !strcmp(host->h_name, "localhost") && host->h_addrtype == AF_INET);
    assert(host->h_length == 4 && host->h_addr_list[0] && !host->h_addr_list[1] && !host->h_aliases[0]);
    assert(!memcmp(host->h_addr_list[0], "\x7f\0\0\x01", 4));

    host = gethostbyname("PS5");
    assert(host && !strcmp(host->h_name, "PS5"));
    assert(!memcmp(host->h_addr_list[0], "\x7f\0\0\x01", 4));

    host = gethostbyname("11.22.33.44");
    assert(host && !strcmp(host->h_name, "11.22.33.44"));
    assert(!memcmp(host->h_addr_list[0], "\x0b\x16\x21\x2c", 4));

    *__h_errno() = 0;
    assert(!gethostbyname("example.com") && *__h_errno() == HOST_NOT_FOUND);
    *__h_errno() = 0;
    assert(!gethostbyname(NULL) && *__h_errno() == HOST_NOT_FOUND);

    /* Reverse lookups name loopback only. */
    assert(inet_pton(AF_INET, "127.0.0.1", &address) == 1);
    host = gethostbyaddr(&address, sizeof(address), AF_INET);
    assert(host && !strcmp(host->h_name, "localhost") && !memcmp(host->h_addr_list[0], &address, 4));
    assert(inet_pton(AF_INET, "11.22.33.44", &address) == 1);
    *__h_errno() = 0;
    assert(!gethostbyaddr(&address, sizeof(address), AF_INET) && *__h_errno() == HOST_NOT_FOUND);
    *__h_errno() = 0;
    assert(!gethostbyaddr(&address, 3, AF_INET) && *__h_errno() == NO_RECOVERY);
}

int main(void)
{
    test_numeric();
    test_null_host();
    test_local_names();
    test_hints();
    test_ports();
    test_free_chain();
    test_getnameinfo();
    test_hostent();
    test_resolver_backend();
    test_negative_cache();
    puts("pw_ws2_32_libc: ok");
    return 0;
}
