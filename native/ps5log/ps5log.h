/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ps5log: single-header TCP/UDP line-stream logger for PS5 homebrew
 * (payloads and native titles). Streams one record per line to a
 * development PC running ps5logd. The destination comes from a dev.conf
 * file so binaries never embed a LAN address.
 *
 * USAGE (single header, stb style)
 *
 *   In exactly one .c/.cpp file, as the FIRST include:
 *       #define PS5LOG_IMPLEMENTATION
 *       #include "ps5log.h"
 *   Everywhere else:
 *       #include "ps5log.h"
 *   (or compile ps5log.c, which is just those two lines, and include the
 *   header normally.)
 *
 *   Minimal program:
 *       int main(void) {
 *           ps5log_quickstart("PPSA99998", "my-app", PS5LOG_CAPTURE_STDIO);
 *           printf("plain printf now reaches the PC too\n");   // RAW line
 *           PS5LOG_LOG("structured line frame=%d", 1);          // seq + timestamp
 *           ps5log_close("done");
 *       }
 *
 * DESIGN RULES
 *   - Never block the caller longer than the configured timeouts.
 *   - Never fail the application: every call degrades to a no-op (or to the
 *     optional mirror file descriptor) when the server is unreachable.
 *   - No heap allocation, no threads. No global signal changes unless the
 *     caller explicitly asks for PS5LOG_CAPTURE_IGNORE_SIGPIPE.
 *   - Not thread-safe by itself; serialize calls from one thread or wrap them.
 *
 * WIRE FORMAT (PROTOCOL.md)
 *   HELLO ps5log/1 title=<id> app=<name> boot=0x<hex> tag=<tag>\n
 *   <seq>\t<mono_ns>\t<level>\t<text>\n          (repeated)
 *   BYE seq=<last> reason=<text>\n
 *   Any other line (e.g. captured printf output) is stored as level RAW.
 */
#ifndef PS5LOG_H
#define PS5LOG_H

#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1   /* POSIX/BSD symbols under -std=c11 on glibc */
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS5LOG_PROTOCOL "ps5log/1"
#define PS5LOG_DEFAULT_PORT 9300
#define PS5LOG_MAX_LINE 1024          /* bytes per record, including "\n" */
#define PS5LOG_CONF_MAX 4096          /* bytes read from a dev.conf file */

/* Conventional levels. Any short token without tabs/newlines is accepted. */
#define PS5LOG_INFO "INFO"
#define PS5LOG_WARN "WARN"
#define PS5LOG_ERR  "ERR"
#define PS5LOG_MARK "MARK"            /* checkpoint / milestone marker */

/* Flags for ps5log_capture_stdio / ps5log_quickstart. */
#define PS5LOG_CAPTURE_NONE           0
#define PS5LOG_CAPTURE_STDOUT         1
#define PS5LOG_CAPTURE_STDERR         2
#define PS5LOG_CAPTURE_STDIO          (PS5LOG_CAPTURE_STDOUT | PS5LOG_CAPTURE_STDERR)
/* Also ignore SIGPIPE process-wide. Only needed on platforms without
 * SO_NOSIGPIPE (Linux hosts); PS5/FreeBSD sockets get SO_NOSIGPIPE. */
#define PS5LOG_CAPTURE_IGNORE_SIGPIPE 4

typedef struct ps5log_config {
    char server[64];                  /* DEV_SERVER: dotted IPv4, empty = disabled */
    uint16_t port;                    /* DEV_PORT, default PS5LOG_DEFAULT_PORT */
    int enabled;                      /* DEV_ENABLED, default 1 */
    int udp;                          /* DEV_UDP: 1 = datagrams, default 0 = TCP */
    int connect_timeout_ms;           /* DEV_CONNECT_TIMEOUT_MS, default 500 */
    int send_timeout_ms;              /* DEV_SEND_TIMEOUT_MS, default 200 */
    char tag[32];                     /* DEV_TAG: free-form, sent in HELLO */
} ps5log_config;

typedef struct ps5log_stats {
    uint64_t lines_sent;              /* records delivered to the socket */
    uint64_t bytes_sent;
    uint64_t lines_dropped;           /* records not sent (disabled or failed) */
    uint64_t lines_mirrored;          /* records written to the mirror fd */
    int last_errno;                   /* errno of the last socket failure */
    int capture_flags;                /* active stdio capture flags */
    const char *disabled_reason;      /* NULL while the network channel is up */
    const char *config_path;          /* dev.conf that was used, or NULL */
} ps5log_stats;

/* Configuration --------------------------------------------------------- */

/* Fill *cfg with defaults (disabled until DEV_SERVER is set). */
void ps5log_config_defaults(ps5log_config *cfg);

/* Apply KEY=VALUE lines from text[0..length). '#' starts a comment. Unknown
 * keys are ignored. Returns the number of recognized keys applied, or -1 if a
 * recognized key has an invalid value (cfg is then partially updated). */
int ps5log_parse_config(const char *text, size_t length, ps5log_config *cfg);

/* Try each path in order; the first readable file is parsed into *cfg and
 * its path stored in *used_path (may be NULL). Returns 0 if a file was read,
 * 1 if none existed, -1 if the first readable file was invalid. */
int ps5log_load_config(const char *const *paths, size_t count,
                       ps5log_config *cfg, const char **used_path);

/* Default search order: /app0/dev.conf, /data/homebrew/dev.conf, ./dev.conf.
 * Override with the PS5LOG_CONF
 * environment variable when present (host testing). */
extern const char *const ps5log_default_conf_paths[];
extern const size_t ps5log_default_conf_path_count;

/* Session ------------------------------------------------------------------ */

/* Connect and send HELLO. Returns 0 when the network channel is up, 1 when
 * logging is disabled or the server is unreachable (mirror still works), and
 * -1 for invalid arguments. Bounded by cfg->connect_timeout_ms. Resets the
 * sequence counter and statistics: one init is one run identity. */
int ps5log_init(const ps5log_config *cfg, const char *title, const char *app,
                uint64_t boot_token);

/* Load the default config search list and call ps5log_init with a monotonic
 * boot token. Same return values as ps5log_init. */
int ps5log_init_default(const char *title, const char *app);

/* One-call setup: ps5log_init_default() followed, when the channel is up, by
 * ps5log_capture_stdio(capture_flags). Returns the init result. */
int ps5log_quickstart(const char *title, const char *app, int capture_flags);

/* Non-zero while the network channel is up. */
int ps5log_enabled(void);

/* Optional compatibility sink. The AGC laboratory forbids filesystem mirrors;
 * this hook remains only for other clients with an explicit durability need. */
void ps5log_set_mirror_fd(int fd);

/* Zero-code-change mode: duplicate the log socket onto fd 1 and/or fd 2 so
 * unchanged printf()/fprintf(stderr)/std::cout output reaches the server as
 * RAW lines (no seq/timestamp). The original descriptors are restored by
 * ps5log_close() and whenever the channel is disabled after a socket
 * failure. Returns 0 on success, 1 if the channel is down (nothing changed),
 * -1 on dup failure. Not for UDP.
 *
 * PRECONDITION for immediate delivery: call this BEFORE the program's first
 * stdout output (ps5log_quickstart as the first statement of main() does).
 * It switches stdout to line buffering with setvbuf(), which the C standard
 * only honours on an unused stream; glibc silently keeps full buffering
 * otherwise, so later printf() lines would only arrive on fflush(stdout),
 * buffer-full or exit. stderr is unbuffered and always immediate. */
int ps5log_capture_stdio(int flags);

/* Restore fd 1/2 to their pre-capture targets. Safe to call repeatedly. */
void ps5log_release_stdio(void);

/* Emit one record. Returns 0 if sent to the network, 1 if only mirrored,
 * -1 if dropped everywhere. Never blocks beyond send_timeout_ms. */
int ps5log_line(const char *level, const char *text);

/* Write bytes to the stream exactly as given (no seq/timestamp framing).
 * For existing loggers that already produce "\n"-terminated text through
 * write(fd, ...), possibly in several fragments per line: TCP preserves the
 * byte order, so the server reassembles lines and stores them as RAW.
 * Nothing is written to the mirror. Returns 0 if sent, -1 if the channel is
 * down or the send failed (the channel is then disabled). */
int ps5log_raw(const void *bytes, size_t length);
int ps5log_printf(const char *level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
int ps5log_hex64(const char *level, const char *label, uint64_t value);

/* Re-establish the network channel after a failure using the stored config
 * and identity; sends a HELLO with resume=1 and re-applies stdio capture if
 * it was active. Returns like ps5log_init. Call at a safe point (end of
 * frame), never inside a GPU transaction. */
int ps5log_reconnect(void);

/* Send BYE (if connected), release captured stdio and close the socket.
 * Safe to call repeatedly. */
void ps5log_close(const char *reason);

/* Introspection ------------------------------------------------------------ */

uint64_t ps5log_monotonic_ns(void);
uint64_t ps5log_next_seq(void);          /* sequence number the next record gets */
void ps5log_stats_get(ps5log_stats *out);

/* Pure formatting helpers (host-testable, no I/O). Both return the number of
 * bytes written excluding the terminating NUL; output always ends with "\n"
 * when capacity >= 2, and never exceeds capacity - 1. */
size_t ps5log_format_record(char *out, size_t capacity, uint64_t seq,
                            uint64_t mono_ns, const char *level,
                            const char *text);
size_t ps5log_format_hello(char *out, size_t capacity, const char *title,
                           const char *app, uint64_t boot_token,
                           const char *tag, int resume, uint64_t next_seq);

/* Convenience macros ------------------------------------------------------- */

#define PS5LOG_LOG(...)   ps5log_printf(PS5LOG_INFO, __VA_ARGS__)
#define PS5LOG_WARNF(...) ps5log_printf(PS5LOG_WARN, __VA_ARGS__)
#define PS5LOG_ERRF(...)  ps5log_printf(PS5LOG_ERR, __VA_ARGS__)
#define PS5LOG_MARKF(...) ps5log_printf(PS5LOG_MARK, __VA_ARGS__)

/* Define PS5LOG_SHORT_MACROS before including to get LOG/LOGW/LOGE/LOGM. */
#ifdef PS5LOG_SHORT_MACROS
#define LOG(...)  PS5LOG_LOG(__VA_ARGS__)
#define LOGW(...) PS5LOG_WARNF(__VA_ARGS__)
#define LOGE(...) PS5LOG_ERRF(__VA_ARGS__)
#define LOGM(...) PS5LOG_MARKF(__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS5LOG_H */

/* ========================================================================= */
/* Implementation                                                             */
/* ========================================================================= */
#ifdef PS5LOG_IMPLEMENTATION
#ifndef PS5LOG_IMPLEMENTATION_DONE
#define PS5LOG_IMPLEMENTATION_DONE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Network backend hooks. Native PS5 titles override these with libSceNet
 * adapters; host programs and payloads retain the POSIX defaults. File and
 * stdio descriptors deliberately continue to use ordinary close(). */
#ifndef PS5LOG_NET_SOCKET
#define PS5LOG_NET_SOCKET socket
#define PS5LOG_NET_CONNECT connect
#define PS5LOG_NET_SEND send
#define PS5LOG_NET_SETSOCKOPT setsockopt
#define PS5LOG_NET_GETSOCKOPT getsockopt
#define PS5LOG_NET_SHUTDOWN shutdown
#define PS5LOG_NET_CLOSE close
#define PS5LOG_NET_FCNTL fcntl
#define PS5LOG_NET_POLL poll
#endif

#ifndef MSG_NOSIGNAL
/* If the platform lacks MSG_NOSIGNAL the caller should ignore SIGPIPE. */
#define MSG_NOSIGNAL 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

const char *const ps5log_default_conf_paths[] = {
    "/app0/dev.conf",
    "/data/homebrew/dev.conf",
    "./dev.conf",
};
const size_t ps5log_default_conf_path_count =
    sizeof(ps5log_default_conf_paths) / sizeof(ps5log_default_conf_paths[0]);

/* ------------------------------------------------------------------------- */
/* Global state (single-threaded by contract)                                */
/* ------------------------------------------------------------------------- */

typedef struct ps5log_state {
    int fd;
    int mirror_fd;
    int initialized;
    int capture_flags;        /* requested flags, re-applied on reconnect */
    int capture_active;
    int saved_stdout;         /* dup() of fd 1 before capture, or -1 */
    int saved_stderr;         /* dup() of fd 2 before capture, or -1 */
    uint64_t seq;
    uint64_t boot_token;
    char title[32];
    char app[48];
    ps5log_config cfg;
    ps5log_stats stats;
    char config_path[128];
} ps5log_state;

static ps5log_state ps5log_g = {
    -1, -1, 0, 0, 0, -1, -1, 0, 0, {0}, {0},
    {{0}, 0, 0, 0, 0, 0, {0}}, {0, 0, 0, 0, 0, 0, NULL, NULL}, {0}
};
#define g ps5log_g

/* ------------------------------------------------------------------------- */
/* Small string helpers (no heap)                                            */
/* ------------------------------------------------------------------------- */

static size_t ps5log__append_bytes(char *out, size_t cap, size_t pos,
                                   const char *src, size_t n) {
    size_t room = (pos < cap) ? cap - 1 - pos : 0;
    if (n > room) n = room;
    memcpy(out + pos, src, n);
    return pos + n;
}

static size_t ps5log__append_str(char *out, size_t cap, size_t pos, const char *src) {
    return ps5log__append_bytes(out, cap, pos, src, strlen(src));
}

static size_t ps5log__append_u64(char *out, size_t cap, size_t pos, uint64_t value) {
    char digits[21];
    size_t n = 0;
    do {
        digits[sizeof(digits) - 1 - n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value);
    return ps5log__append_bytes(out, cap, pos, digits + sizeof(digits) - n, n);
}

static size_t ps5log__append_hex64(char *out, size_t cap, size_t pos, uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    size_t n = 0;
    do {
        digits[sizeof(digits) - 1 - n++] = hex[value & 0xf];
        value >>= 4;
    } while (value);
    pos = ps5log__append_str(out, cap, pos, "0x");
    return ps5log__append_bytes(out, cap, pos, digits + sizeof(digits) - n, n);
}

/* Copy src into out[pos..] replacing bytes that would break framing. */
static size_t ps5log__append_sanitized(char *out, size_t cap, size_t pos,
                                       const char *src, int allow_tab) {
    for (; *src; ++src) {
        char c = *src;
        if (c == '\n' || c == '\r' || (!allow_tab && c == '\t') ||
            (!allow_tab && c == ' '))
            c = allow_tab ? ' ' : '_';
        if (pos + 1 >= cap) break;
        out[pos++] = c;
    }
    return pos;
}

static size_t ps5log__finish_line(char *out, size_t cap, size_t pos) {
    if (cap == 0) return 0;
    if (cap == 1) { out[0] = 0; return 0; }
    if (pos > cap - 2) pos = cap - 2;
    out[pos++] = '\n';
    out[pos] = 0;
    return pos;
}

static void ps5log__copy_bounded(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src ? src : "", n);
    dst[n] = 0;
}

/* ------------------------------------------------------------------------- */
/* Formatting (pure)                                                          */
/* ------------------------------------------------------------------------- */

size_t ps5log_format_record(char *out, size_t cap, uint64_t seq,
                            uint64_t mono_ns, const char *level,
                            const char *text) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    if (!level || !*level) level = PS5LOG_INFO;
    pos = ps5log__append_u64(out, cap, pos, seq);
    pos = ps5log__append_str(out, cap, pos, "\t");
    pos = ps5log__append_u64(out, cap, pos, mono_ns);
    pos = ps5log__append_str(out, cap, pos, "\t");
    pos = ps5log__append_sanitized(out, cap, pos, level, 0);
    pos = ps5log__append_str(out, cap, pos, "\t");
    pos = ps5log__append_sanitized(out, cap, pos, text ? text : "", 1);
    return ps5log__finish_line(out, cap, pos);
}

size_t ps5log_format_hello(char *out, size_t cap, const char *title,
                           const char *app, uint64_t boot_token,
                           const char *tag, int resume, uint64_t next_seq) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    pos = ps5log__append_str(out, cap, pos, "HELLO " PS5LOG_PROTOCOL " title=");
    pos = ps5log__append_sanitized(out, cap, pos, title && *title ? title : "unknown", 0);
    pos = ps5log__append_str(out, cap, pos, " app=");
    pos = ps5log__append_sanitized(out, cap, pos, app && *app ? app : "unknown", 0);
    pos = ps5log__append_str(out, cap, pos, " boot=");
    pos = ps5log__append_hex64(out, cap, pos, boot_token);
    if (tag && *tag) {
        pos = ps5log__append_str(out, cap, pos, " tag=");
        pos = ps5log__append_sanitized(out, cap, pos, tag, 0);
    }
    if (resume) {
        pos = ps5log__append_str(out, cap, pos, " resume=1 next_seq=");
        pos = ps5log__append_u64(out, cap, pos, next_seq);
    }
    return ps5log__finish_line(out, cap, pos);
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                              */
/* ------------------------------------------------------------------------- */

void ps5log_config_defaults(ps5log_config *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = PS5LOG_DEFAULT_PORT;
    cfg->enabled = 1;
    cfg->udp = 0;
    cfg->connect_timeout_ms = 500;
    cfg->send_timeout_ms = 200;
}

static int ps5log__parse_int(const char *value, size_t len, long min, long max,
                             long *out) {
    char buf[16];
    char *end = NULL;
    long parsed;
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, value, len);
    buf[len] = 0;
    parsed = strtol(buf, &end, 10);
    if (!end || *end || parsed < min || parsed > max) return -1;
    *out = parsed;
    return 0;
}

static const char *ps5log__trim(const char *start, const char **end_out,
                                const char *end) {
    while (start < end && (*start == ' ' || *start == '\t')) ++start;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
    *end_out = end;
    return start;
}

static int ps5log__apply_key(ps5log_config *cfg, const char *key, size_t klen,
                             const char *value, size_t vlen) {
    long number;
#define PS5LOG__KEY_IS(name) (klen == sizeof(name) - 1 && memcmp(key, name, klen) == 0)
    if (PS5LOG__KEY_IS("DEV_SERVER")) {
        if (vlen >= sizeof(cfg->server)) return -1;
        memcpy(cfg->server, value, vlen);
        cfg->server[vlen] = 0;
        return 1;
    }
    if (PS5LOG__KEY_IS("DEV_PORT")) {
        if (ps5log__parse_int(value, vlen, 1, 65535, &number)) return -1;
        cfg->port = (uint16_t)number;
        return 1;
    }
    if (PS5LOG__KEY_IS("DEV_ENABLED")) {
        if (ps5log__parse_int(value, vlen, 0, 1, &number)) return -1;
        cfg->enabled = (int)number;
        return 1;
    }
    if (PS5LOG__KEY_IS("DEV_UDP")) {
        if (ps5log__parse_int(value, vlen, 0, 1, &number)) return -1;
        cfg->udp = (int)number;
        return 1;
    }
    if (PS5LOG__KEY_IS("DEV_CONNECT_TIMEOUT_MS")) {
        if (ps5log__parse_int(value, vlen, 0, 60000, &number)) return -1;
        cfg->connect_timeout_ms = (int)number;
        return 1;
    }
    if (PS5LOG__KEY_IS("DEV_SEND_TIMEOUT_MS")) {
        if (ps5log__parse_int(value, vlen, 0, 60000, &number)) return -1;
        cfg->send_timeout_ms = (int)number;
        return 1;
    }
    if (PS5LOG__KEY_IS("DEV_TAG")) {
        if (vlen >= sizeof(cfg->tag)) vlen = sizeof(cfg->tag) - 1;
        memcpy(cfg->tag, value, vlen);
        cfg->tag[vlen] = 0;
        return 1;
    }
#undef PS5LOG__KEY_IS
    return 0; /* unknown key: ignored */
}

int ps5log_parse_config(const char *text, size_t length, ps5log_config *cfg) {
    const char *cursor = text;
    const char *end = text + length;
    int applied = 0;
    if (!text || !cfg) return -1;
    while (cursor < end) {
        const char *line_end = (const char *)memchr(cursor, '\n', (size_t)(end - cursor));
        const char *line_stop = line_end ? line_end : end;
        const char *trimmed_end;
        const char *line = ps5log__trim(cursor, &trimmed_end, line_stop);
        cursor = line_end ? line_end + 1 : end;
        if (line >= trimmed_end || *line == '#') continue;
        {
            const char *eq = (const char *)memchr(line, '=', (size_t)(trimmed_end - line));
            const char *key_end, *value_end;
            const char *key, *value;
            int rc;
            if (!eq) return -1;
            key = ps5log__trim(line, &key_end, eq);
            value = ps5log__trim(eq + 1, &value_end, trimmed_end);
            if (key >= key_end) return -1;
            rc = ps5log__apply_key(cfg, key, (size_t)(key_end - key), value,
                                   (size_t)(value_end - value));
            if (rc < 0) return -1;
            applied += rc;
        }
    }
    return applied;
}

int ps5log_load_config(const char *const *paths, size_t count,
                       ps5log_config *cfg, const char **used_path) {
    size_t i;
    if (used_path) *used_path = NULL;
    if (!paths || !cfg) return -1;
    for (i = 0; i < count; ++i) {
        char buffer[PS5LOG_CONF_MAX];
        ssize_t got;
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) continue;
        got = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (got < 0) continue;
        if (used_path) *used_path = paths[i];
        return ps5log_parse_config(buffer, (size_t)got, cfg) < 0 ? -1 : 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Time                                                                       */
/* ------------------------------------------------------------------------- */

uint64_t ps5log_monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------- */
/* stdio capture                                                              */
/* ------------------------------------------------------------------------- */

void ps5log_release_stdio(void) {
    if (!g.capture_active) return;
    fflush(stdout);
    fflush(stderr);
    if (g.saved_stdout >= 0) {
        (void)dup2(g.saved_stdout, STDOUT_FILENO);
        close(g.saved_stdout);
        g.saved_stdout = -1;
    }
    if (g.saved_stderr >= 0) {
        (void)dup2(g.saved_stderr, STDERR_FILENO);
        close(g.saved_stderr);
        g.saved_stderr = -1;
    }
    g.capture_active = 0;
    g.stats.capture_flags = 0;
}

static int ps5log__apply_capture(int flags) {
    int applied = 0;
    if (g.fd < 0 || g.cfg.udp) return 1;
    if (flags & PS5LOG_CAPTURE_IGNORE_SIGPIPE) {
        (void)signal(SIGPIPE, SIG_IGN);
    }
    if (flags & PS5LOG_CAPTURE_STDOUT) {
        fflush(stdout);
        if (g.saved_stdout < 0) g.saved_stdout = dup(STDOUT_FILENO);
        if (g.saved_stdout < 0 || dup2(g.fd, STDOUT_FILENO) < 0) return -1;
        (void)setvbuf(stdout, NULL, _IOLBF, 0);
        applied |= PS5LOG_CAPTURE_STDOUT;
    }
    if (flags & PS5LOG_CAPTURE_STDERR) {
        fflush(stderr);
        if (g.saved_stderr < 0) g.saved_stderr = dup(STDERR_FILENO);
        if (g.saved_stderr < 0 || dup2(g.fd, STDERR_FILENO) < 0) return -1;
        applied |= PS5LOG_CAPTURE_STDERR;
    }
    g.capture_active = applied != 0;
    g.stats.capture_flags = applied | (flags & PS5LOG_CAPTURE_IGNORE_SIGPIPE);
    return 0;
}

int ps5log_capture_stdio(int flags) {
    int rc;
    g.capture_flags = flags;
    rc = ps5log__apply_capture(flags);
    if (rc < 0) ps5log_release_stdio();
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Socket plumbing                                                            */
/* ------------------------------------------------------------------------- */

static void ps5log__disable_channel(const char *reason, int err) {
    ps5log_release_stdio();
    if (g.fd >= 0) {
        PS5LOG_NET_CLOSE(g.fd);
        g.fd = -1;
    }
    g.stats.disabled_reason = reason;
    if (err) g.stats.last_errno = err;
}

/* Dotted-quad parser: avoids inet_pton()/htons() so the client adds no
 * network-library import beyond the socket syscalls themselves. Writes the
 * address and port in network byte order directly. Returns 0 on success. */
static int ps5log__fill_ipv4(struct sockaddr_in *addr, const char *text,
                             uint16_t port) {
    unsigned char *ip = (unsigned char *)&addr->sin_addr;
    unsigned char *port_bytes = (unsigned char *)&addr->sin_port;
    int octet;
    for (octet = 0; octet < 4; ++octet) {
        int value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (*text - '0');
            if (value > 255 || ++digits > 3) return -1;
            ++text;
        }
        if (digits == 0) return -1;
        ip[octet] = (unsigned char)value;
        if (octet < 3) {
            if (*text != '.') return -1;
            ++text;
        }
    }
    if (*text != 0) return -1;
    port_bytes[0] = (unsigned char)(port >> 8);
    port_bytes[1] = (unsigned char)(port & 0xff);
    return 0;
}

static int ps5log__open_channel(const ps5log_config *cfg) {
    struct sockaddr_in addr;
    int fd, flags, rc;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (ps5log__fill_ipv4(&addr, cfg->server, cfg->port) != 0) {
        ps5log__disable_channel("DEV_SERVER is not a dotted IPv4 address", 0);
        return -1;
    }
    fd = PS5LOG_NET_SOCKET(AF_INET, cfg->udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) {
        ps5log__disable_channel("socket() failed", errno);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        (void)PS5LOG_NET_SETSOCKOPT(
            fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    if (cfg->udp) {
        if (PS5LOG_NET_CONNECT(
                fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            int err = errno;
            PS5LOG_NET_CLOSE(fd);
            ps5log__disable_channel("udp connect() failed", err);
            return -1;
        }
        return fd;
    }
    flags = PS5LOG_NET_FCNTL(fd, F_GETFL, 0);
    if (flags < 0 || PS5LOG_NET_FCNTL(
            fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int err = errno;
        PS5LOG_NET_CLOSE(fd);
        ps5log__disable_channel("fcntl() failed", err);
        return -1;
    }
    rc = PS5LOG_NET_CONNECT(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
        int err = errno;
        PS5LOG_NET_CLOSE(fd);
        ps5log__disable_channel("connect() refused", err);
        return -1;
    }
    if (rc != 0) {
        struct pollfd pfd;
        int err = 0;
        socklen_t len = sizeof(err);
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        rc = PS5LOG_NET_POLL(&pfd, 1, cfg->connect_timeout_ms);
        if (rc <= 0) {
            err = rc == 0 ? ETIMEDOUT : errno;
            PS5LOG_NET_CLOSE(fd);
            ps5log__disable_channel("connect() timed out", err);
            return -1;
        }
        if (PS5LOG_NET_GETSOCKOPT(
                fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            if (err == 0) err = errno;
            PS5LOG_NET_CLOSE(fd);
            ps5log__disable_channel("connect() failed", err);
            return -1;
        }
    }
    (void)PS5LOG_NET_FCNTL(fd, F_SETFL, flags);
    {
        struct timeval tv;
        int one = 1;
        tv.tv_sec = cfg->send_timeout_ms / 1000;
        tv.tv_usec = (cfg->send_timeout_ms % 1000) * 1000;
        (void)PS5LOG_NET_SETSOCKOPT(
            fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        (void)PS5LOG_NET_SETSOCKOPT(
            fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

static int ps5log__send_all(const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = PS5LOG_NET_SEND(
            g.fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        sent += (size_t)n;
    }
    g.stats.bytes_sent += length;
    return 0;
}

static int ps5log__send_hello(int resume) {
    char line[256];
    size_t n = ps5log_format_hello(line, sizeof(line), g.title, g.app,
                                   g.boot_token, g.cfg.tag, resume, g.seq + 1);
    if (ps5log__send_all(line, n) != 0) {
        ps5log__disable_channel("HELLO send failed", errno);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public session API                                                         */
/* ------------------------------------------------------------------------- */

int ps5log_init(const ps5log_config *cfg, const char *title, const char *app,
                uint64_t boot_token) {
    if (!cfg) return -1;
    if (g.fd >= 0) ps5log_close("reinit");
    {
        /* A new session identity starts a new sequence and fresh counters. */
        const char *config_path = g.stats.config_path;
        memset(&g.stats, 0, sizeof(g.stats));
        g.stats.config_path = config_path;
        g.seq = 0;
    }
    g.cfg = *cfg;
    ps5log__copy_bounded(g.title, sizeof(g.title), title);
    ps5log__copy_bounded(g.app, sizeof(g.app), app);
    g.boot_token = boot_token;
    g.initialized = 1;
    g.stats.disabled_reason = NULL;
    if (!cfg->enabled) {
        ps5log__disable_channel("DEV_ENABLED=0", 0);
        return 1;
    }
    if (!cfg->server[0]) {
        ps5log__disable_channel("no DEV_SERVER configured", 0);
        return 1;
    }
    g.fd = ps5log__open_channel(&g.cfg);
    if (g.fd < 0) return 1;
    return ps5log__send_hello(0) == 0 ? 0 : 1;
}

int ps5log_init_default(const char *title, const char *app) {
    ps5log_config cfg;
    const char *used = NULL;
    const char *env = getenv("PS5LOG_CONF");
    int rc;
    ps5log_config_defaults(&cfg);
    if (env && *env) {
        const char *paths[1];
        paths[0] = env;
        rc = ps5log_load_config(paths, 1, &cfg, &used);
    } else {
        rc = ps5log_load_config(ps5log_default_conf_paths,
                                ps5log_default_conf_path_count, &cfg, &used);
    }
    ps5log__copy_bounded(g.config_path, sizeof(g.config_path), used);
    g.stats.config_path = used ? g.config_path : NULL;
    if (rc < 0) {
        ps5log_config_defaults(&cfg);
        cfg.enabled = 0;
        (void)ps5log_init(&cfg, title, app, ps5log_monotonic_ns());
        ps5log__disable_channel("dev.conf is invalid", 0);
        return 1;
    }
    return ps5log_init(&cfg, title, app, ps5log_monotonic_ns());
}

int ps5log_quickstart(const char *title, const char *app, int capture_flags) {
    int rc = ps5log_init_default(title, app);
    if (rc == 0 && capture_flags) (void)ps5log_capture_stdio(capture_flags);
    return rc;
}

int ps5log_enabled(void) {
    return g.fd >= 0;
}

void ps5log_set_mirror_fd(int fd) {
    g.mirror_fd = fd;
}

int ps5log_line(const char *level, const char *text) {
    char line[PS5LOG_MAX_LINE];
    size_t n;
    int mirrored = 0;
    uint64_t seq = ++g.seq;
    n = ps5log_format_record(line, sizeof(line), seq, ps5log_monotonic_ns(),
                             level, text);
    if (g.mirror_fd >= 0) {
        size_t done = 0;
        while (done < n) {
            ssize_t w = write(g.mirror_fd, line + done, n - done);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                break;
            }
            done += (size_t)w;
        }
        mirrored = done == n;
        if (mirrored) g.stats.lines_mirrored++;
    }
    if (g.fd < 0) {
        g.stats.lines_dropped++;
        return mirrored ? 1 : -1;
    }
    if (ps5log__send_all(line, n) != 0) {
        int err = errno;
        g.stats.lines_dropped++;
        ps5log__disable_channel(err == EAGAIN || err == EWOULDBLOCK
                                    ? "send() timed out" : "send() failed", err);
        return mirrored ? 1 : -1;
    }
    g.stats.lines_sent++;
    return 0;
}

int ps5log_raw(const void *bytes, size_t length) {
    if (g.fd < 0 || !bytes) return -1;
    if (length == 0) return 0;
    if (ps5log__send_all((const char *)bytes, length) != 0) {
        int err = errno;
        ps5log__disable_channel(err == EAGAIN || err == EWOULDBLOCK
                                    ? "send() timed out" : "send() failed", err);
        return -1;
    }
    return 0;
}

int ps5log_printf(const char *level, const char *fmt, ...) {
    char text[PS5LOG_MAX_LINE];
    va_list args;
    int n;
    if (!fmt) return ps5log_line(level, "");
    va_start(args, fmt);
    n = vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (n < 0) text[0] = 0;
    return ps5log_line(level, text);
}

int ps5log_hex64(const char *level, const char *label, uint64_t value) {
    char text[PS5LOG_MAX_LINE];
    size_t pos = 0;
    pos = ps5log__append_sanitized(text, sizeof(text), pos, label ? label : "value", 1);
    pos = ps5log__append_str(text, sizeof(text), pos, "=");
    pos = ps5log__append_hex64(text, sizeof(text), pos, value);
    text[pos] = 0;
    return ps5log_line(level, text);
}

int ps5log_reconnect(void) {
    if (!g.initialized) return -1;
    if (g.fd >= 0) return 0;
    if (!g.cfg.enabled || !g.cfg.server[0]) return 1;
    g.stats.disabled_reason = NULL;
    g.fd = ps5log__open_channel(&g.cfg);
    if (g.fd < 0) return 1;
    if (ps5log__send_hello(1) != 0) return 1;
    if (g.capture_flags) (void)ps5log__apply_capture(g.capture_flags);
    return 0;
}

void ps5log_close(const char *reason) {
    ps5log_release_stdio();
    if (g.fd >= 0) {
        char line[192];
        size_t pos = 0;
        pos = ps5log__append_str(line, sizeof(line), pos, "BYE seq=");
        pos = ps5log__append_u64(line, sizeof(line), pos, g.seq);
        pos = ps5log__append_str(line, sizeof(line), pos, " reason=");
        pos = ps5log__append_sanitized(line, sizeof(line), pos,
                                       reason && *reason ? reason : "closed", 0);
        pos = ps5log__finish_line(line, sizeof(line), pos);
        (void)ps5log__send_all(line, pos);
        (void)PS5LOG_NET_SHUTDOWN(g.fd, SHUT_WR);
        PS5LOG_NET_CLOSE(g.fd);
        g.fd = -1;
        g.stats.disabled_reason = "closed";
    }
}

uint64_t ps5log_next_seq(void) {
    return g.seq + 1;
}

void ps5log_stats_get(ps5log_stats *out) {
    if (out) *out = g.stats;
}

#undef g

#ifdef __cplusplus
}
#endif

#endif /* PS5LOG_IMPLEMENTATION_DONE */
#endif /* PS5LOG_IMPLEMENTATION */
