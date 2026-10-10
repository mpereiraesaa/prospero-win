/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_diagnostics.h"
#include "ps5log/ps5log.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A record costs the thread that logs it a format and a copy into the
 * pending buffer under `lock`, never a write(), rename() or send(): Wine's
 * stderr sink calls pw_diagnostics_log() from any game thread, and the
 * console's filesystem takes milliseconds to rotate a chunk. The writer
 * thread takes the full buffer under the lock, swaps in the empty one, and
 * saves and sends it with the lock released; while it does (`writing`) the
 * session file is its own, and open/close/resume wait for it before they
 * touch the descriptor. A full buffer drops records rather than wait, and
 * the writer says how many once it has caught up.
 *
 * `lock` guards the two buffers, the counters and the writer's state and is
 * only ever held for memory copies. `net_lock` guards the TCP channel
 * (ps5log), whose sends and reconnects may wait for the network. No caller
 * holds both, so a slow network never stalls a caller that only writes the
 * buffer, such as the title's loop calling pw_diagnostics_tick(). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;    /* records to write, or a pass asked */
static pthread_cond_t drained = PTHREAD_COND_INITIALIZER; /* the writer finished a pass */
static volatile sig_atomic_t fd = -1;
static int disk_error; /* __atomic: the writer stores it with the lock released */
static char current[512], previous[512], identity[256];
static char buffers[2][PW_DIAGNOSTICS_PENDING];
static char *pending = buffers[0], *draining = buffers[1];
static size_t used, bytes;
static uint64_t sequence, last_signal, dropped_records, dropped_bytes, dropped_total;
static int writing, writer_started, writer_refused;
/* Set while the retry thread reconnects: records then skip the network (they
 * are still saved) instead of waiting for the connect timeout. */
static int reconnecting; /* __atomic: written by the retry thread */
static int retry_started;
static uint64_t retry_interval_ns = 5000000000ull;
/* The space a chunk keeps for the signal path: pw_diagnostics_emergency()
 * writes up to 164 bytes past what the writer counted. */
enum { PW_DIAGNOSTICS_RESERVE = 192, PW_DIAGNOSTICS_CADENCE_NS = 100000000 };

static int put_all(int target, const char *data, size_t length)
{
    while (length) {
        ssize_t n = write(target, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n; length -= (size_t)n;
    }
    return 0;
}

/* Lock held. The sequence counts dropped records too, so a gap in the saved
 * numbers is a drop and the drop record says how many. */
static size_t format_record(char *out, size_t capacity, const char *message)
{
    struct timespec clock;
    clock_gettime(CLOCK_MONOTONIC, &clock);
    int n = snprintf(out, capacity, "REC seq=%llu t=%lld.%09ld %s\n",
                     (unsigned long long)++sequence, (long long)clock.tv_sec, clock.tv_nsec, message);
    return n > 0 ? (size_t)n : 0;
}

/* Lock held: records to save or send, and a file to save them in (or none
 * ever will be: a failed disk still lets them reach the network). */
static int ready(void)
{
    return used && (fd >= 0 || __atomic_load_n(&disk_error, __ATOMIC_ACQUIRE));
}

static void disk_failed(int error)
{
    __atomic_store_n(&disk_error, error ? error : EIO, __ATOMIC_RELEASE);
    if (fd >= 0) close(fd);
    fd = -1;
}

/* Writer thread only. The current chunk becomes the previous one and a new
 * one starts with the identity line; records stay in order across them. */
static int rotate(void)
{
    close(fd); fd = -1;
    if (rename(current, previous) != 0) { disk_failed(errno); return -1; }
    fd = open(current, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bytes = strlen(identity);
    if (fd < 0 || put_all(fd, identity, bytes) != 0) { disk_failed(errno); return -1; }
    return 0;
}

/* Writer thread only: whole lines, rotating before one that would take the
 * chunk past its size (a line longer than a chunk goes in as it is). */
static void save(const char *data, size_t length)
{
    size_t at = 0, run = 0;
    if (fd < 0) return;
    while (at < length) {
        const char *end = memchr(data + at, '\n', length - at);
        size_t line = end ? (size_t)(end - (data + at)) + 1 : length - at;
        if (bytes + run + line > PW_DIAGNOSTICS_CHUNK - PW_DIAGNOSTICS_RESERVE && bytes + run > strlen(identity)) {
            if (put_all(fd, data + at - run, run) != 0) { disk_failed(errno); return; }
            bytes += run; run = 0;
            if (rotate() != 0) return;
        }
        run += line; at += line;
    }
    if (run && put_all(fd, data + at - run, run) != 0) { disk_failed(errno); return; }
    bytes += run;
}

/* Writer thread only: each record's message, without the REC prefix, as
 * its own ps5log record. */
static void send_records(const char *data, size_t length)
{
    char message[PS5LOG_MAX_LINE + 96];
    size_t at = 0;
    if (__atomic_load_n(&reconnecting, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&net_lock);
    while (at < length) {
        const char *end = memchr(data + at, '\n', length - at);
        size_t line = end ? (size_t)(end - (data + at)) : length - at;
        const char *text = data + at;
        size_t skip = 0;
        for (int spaces = 0; skip < line && spaces < 3; skip++) spaces += text[skip] == ' ';
        size_t n = line - skip < sizeof(message) ? line - skip : sizeof(message) - 1;
        memcpy(message, text + skip, n); message[n] = 0;
        (void)ps5log_line(PS5LOG_INFO, message);
        at += line + (end != NULL);
    }
    pthread_mutex_unlock(&net_lock);
}

/* Lock held on entry and return. Takes the pending buffer and saves and
 * sends it with the lock released. */
static void pass(void)
{
    char *full = pending;
    size_t length = used;
    pending = draining; draining = full; used = 0;
    if (dropped_records) {
        char text[128];
        snprintf(text, sizeof(text), "PW_WINE64 diagnostics dropped records=%llu bytes=%llu",
                 (unsigned long long)dropped_records, (unsigned long long)dropped_bytes);
        used = format_record(pending, PW_DIAGNOSTICS_PENDING, text);
        dropped_records = dropped_bytes = 0;
    }
    writing = 1;
    pthread_mutex_unlock(&lock);
    save(draining, length);
    send_records(draining, length);
    pthread_mutex_lock(&lock);
    writing = 0;
    pthread_cond_broadcast(&drained);
}

/* Saves and sends records every 100 ms, when a pass is asked, or when the
 * buffer is half full, whichever comes first. */
static void *write_records(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!ready()) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += PW_DIAGNOSTICS_CADENCE_NS;
            if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            (void)pthread_cond_timedwait(&wake, &lock, &until);
        }
        pass();
    }
    return NULL;
}

/* Lock held. Waits until every record that can be saved is, and the writer
 * has let go of the session file. Without a writer thread (its start
 * failed) the caller saves them itself. */
static void wait_for_writer(void)
{
    if (!writer_started) { while (ready()) pass(); return; }
    pthread_cond_signal(&wake);
    while (writing || ready()) pthread_cond_wait(&drained, &lock);
}

static void sleep_ns(uint64_t ns)
{
    struct timespec delay = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

/* Brings the TCP channel back after it drops. A reconnect waits up to the
 * channel's connect timeout, so it runs here and never in the title's loop. */
static void *retry_network(void *unused)
{
    (void)unused;
    for (;;) {
        sleep_ns(retry_interval_ns);
        pthread_mutex_lock(&net_lock);
        int up = ps5log_enabled();
        pthread_mutex_unlock(&net_lock);
        if (up) continue;
        __atomic_store_n(&reconnecting, 1, __ATOMIC_RELEASE);
        pthread_mutex_lock(&net_lock);
        (void)ps5log_reconnect();
        pthread_mutex_unlock(&net_lock);
        __atomic_store_n(&reconnecting, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

static int start_thread(void *(*entry)(void *))
{
    pthread_attr_t attributes;
    pthread_t thread;
    int status;

    if (pthread_attr_init(&attributes) != 0) return -1;
    (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    status = pthread_create(&thread, &attributes, entry, NULL);
    pthread_attr_destroy(&attributes);
    return status == 0 ? 0 : -1;
}

/* Lock held. Both threads start with the session: records before it only
 * wait in the buffer. */
static void start_threads(void)
{
    if (!retry_started && start_thread(retry_network) == 0) retry_started = 1;
    if (!writer_started && !writer_refused && start_thread(write_records) == 0) writer_started = 1;
    pthread_cond_signal(&wake);
}

int pw_diagnostics_open(const char *root, const char *build, const char *profile, uint32_t cycle)
{
    char directory[448], index[512], number[32];
    unsigned next = 0;
    int counter, result = -1;
    pthread_mutex_lock(&lock);
    wait_for_writer();
    if (fd >= 0) { close(fd); fd = -1; }
    disk_error = 0;
    if (!root || snprintf(directory, sizeof(directory), "%s/logs", root) >= (int)sizeof(directory)) {
        disk_error = ENAMETOOLONG; goto out;
    }
    if (mkdir(root, 0700) != 0 && errno != EEXIST) { disk_error = errno; goto out; }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) { disk_error = errno; goto out; }
    snprintf(index, sizeof(index), "%s/next.txt", directory);
    counter = open(index, O_RDONLY);
    if (counter >= 0) {
        ssize_t n = read(counter, number, sizeof(number) - 1); close(counter);
        if (n > 0) { number[n] = 0; (void)sscanf(number, "%u", &next); }
    }
    next %= PW_DIAGNOSTICS_SESSIONS;
    snprintf(current, sizeof(current), "%s/session-%u.log", directory, next);
    snprintf(previous, sizeof(previous), "%s/session-%u.previous.log", directory, next);
    (void)unlink(previous);
    fd = open(current, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { disk_error = errno; goto out; }
    /* Identity repeats on rotation so a retained tail can be identified. */
    snprintf(identity, sizeof(identity), "PW_REPORT/1 build=%s profile=%s cycle=%u pid=%ld time=%lld\n",
             build ? build : "unknown", profile ? profile : "launcher", cycle,
             (long)getpid(), (long long)time(NULL));
    bytes = strlen(identity);
    if (put_all(fd, identity, bytes) != 0) { disk_error = errno; close(fd); fd = -1; goto out; }
    counter = open(index, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (counter < 0) { disk_error = errno; close(fd); fd = -1; goto out; }
    int n = snprintf(number, sizeof(number), "%u\n", (next + 1) % PW_DIAGNOSTICS_SESSIONS);
    int saved = put_all(counter, number, (size_t)n);
    close(counter);
    if (saved != 0) { disk_error = errno; close(fd); fd = -1; goto out; }
    result = 0;
out:
    start_threads();
    pthread_mutex_unlock(&lock);
    return result;
}

void pw_diagnostics_log(const char *format, ...)
{
    /* The same capacity as ps5log's own records; a longer message is cut and
     * says so, in the saved file and on the network alike. */
    static const char marker[] = " [truncated]";
    char message[PS5LOG_MAX_LINE];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (length < 0) message[0] = 0;
    else if ((size_t)length >= sizeof(message))
        memcpy(message + sizeof(message) - sizeof(marker), marker, sizeof(marker));
    pthread_mutex_lock(&lock);
    size_t size = format_record(pending + used, PW_DIAGNOSTICS_PENDING - used, message);
    if (used + size < PW_DIAGNOSTICS_PENDING) {
        size_t before = used;
        used += size;
        if (before < PW_DIAGNOSTICS_PENDING / 2 && used >= PW_DIAGNOSTICS_PENDING / 2) pthread_cond_signal(&wake);
    } else {
        /* Full: the writer is behind the records (a slow disk or network).
         * Dropping keeps the caller, a game thread, out of the wait. */
        dropped_records++; dropped_total++; dropped_bytes += size;
    }
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_tick(uint64_t now)
{
    pthread_mutex_lock(&lock);
    if (now - last_signal >= PW_DIAGNOSTICS_CADENCE_NS) { pthread_cond_signal(&wake); last_signal = now; }
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_close(const char *reason)
{
    pw_diagnostics_log("PW_WINE64 session_end reason=%s", reason);
    pthread_mutex_lock(&lock);
    wait_for_writer();
    if (fd >= 0) { (void)fsync(fd); close(fd); fd = -1; }
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&net_lock);
    ps5log_close(reason);
    pthread_mutex_unlock(&net_lock);
}

void pw_diagnostics_resume(void)
{
    pthread_mutex_lock(&lock);
    wait_for_writer();
    if (fd < 0 && current[0]) {
        fd = open(current, O_WRONLY | O_APPEND);
        if (fd < 0) disk_error = errno;
        else pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    /* The title is still running after a failed restart: bring the channel
     * back now, as before closing it, rather than in the retry thread's time. */
    pthread_mutex_lock(&net_lock);
    (void)ps5log_reconnect();
    pthread_mutex_unlock(&net_lock);
}

void pw_diagnostics_status(char *buffer, size_t size)
{
    pthread_mutex_lock(&lock);
    int error = __atomic_load_n(&disk_error, __ATOMIC_ACQUIRE);
    /* Mid-rotation the descriptor is briefly closed; the file is still there. */
    if (fd >= 0 || (writing && !error)) {
        if (dropped_total)
            snprintf(buffer, size, "LOG SAVED (%llu DROPPED) - OPTIONS: REPORT HELP", (unsigned long long)dropped_total);
        else
            snprintf(buffer, size, "LOG SAVED - OPTIONS: REPORT HELP");
    } else {
        snprintf(buffer, size, "LOG UNAVAILABLE (ERROR %d)", error);
    }
    pthread_mutex_unlock(&lock);
}

static size_t append_text(char *line, size_t n, const char *text)
{
    while (*text) line[n++] = *text++;
    return n;
}

static size_t append_hex(char *line, size_t n, uint64_t value)
{
    for (int shift = 60; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(value >> shift) & 15];
    return n;
}

void pw_diagnostics_emergency(int signal, uintptr_t address, uint64_t pc,
                              int segment, uint64_t segment_offset)
{
    char line[192];
    size_t n = 0;

    n = append_text(line, n, "PW_WINE64 fault signal=0x");
    n = append_hex(line, n, (unsigned)signal);
    n = append_text(line, n, " addr=0x");
    n = append_hex(line, n, address);
    n = append_text(line, n, " rip=0x");
    n = append_hex(line, n, pc);
    if (segment >= 0) {
        n = append_text(line, n, " ntdll_segment=0x");
        n = append_hex(line, n, (unsigned)segment);
        n = append_text(line, n, " offset=0x");
        n = append_hex(line, n, segment_offset);
    }
    line[n++] = '\n';
    /* write() and send() only: no lock, no allocation, no formatting library. */
    if (fd >= 0) { ssize_t result = write(fd, line, n); (void)result; }
    (void)ps5log_raw(line, n);
}

#ifdef PW_DIAGNOSTICS_TESTING
void pw_diagnostics_test_retry_interval(uint64_t ns) { retry_interval_ns = ns; }
void pw_diagnostics_test_refuse_writer(int refuse) { writer_refused = refuse; }
void pw_diagnostics_test_settle(void)
{
    pthread_mutex_lock(&lock);
    wait_for_writer();
    pthread_mutex_unlock(&lock);
}
#endif
