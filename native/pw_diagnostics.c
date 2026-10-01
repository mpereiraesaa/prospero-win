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

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t fd = -1;
static int disk_error;
static char current[512], previous[512], identity[256];
static char pending[65536];
static size_t used, bytes;
static uint64_t sequence, last_flush, last_retry;

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

static void flush(void)
{
    if (fd < 0 || !used) return;
    if (bytes + used > PW_DIAGNOSTICS_CHUNK - 128u) {
        close(fd); fd = -1;
        if (rename(current, previous) != 0) { disk_error = errno; used = 0; return; }
        fd = open(current, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        bytes = strlen(identity);
        if (fd < 0 || put_all(fd, identity, bytes) != 0) goto failed;
    }
    if (put_all(fd, pending, used) != 0) goto failed;
    bytes += used; used = 0;
    return;
failed:
    disk_error = errno ? errno : EIO;
    if (fd >= 0) close(fd);
    fd = -1; used = 0;
}

int pw_diagnostics_open(const char *root, const char *build, const char *profile, uint32_t cycle)
{
    char directory[448], index[512], number[32];
    unsigned next = 0;
    int counter, result = -1;
    pthread_mutex_lock(&lock);
    if (fd >= 0) { flush(); close(fd); fd = -1; }
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
    flush(); result = fd >= 0 ? 0 : -1;
out:
    pthread_mutex_unlock(&lock);
    return result;
}

void pw_diagnostics_log(const char *format, ...)
{
    char message[768], line[1024];
    struct timespec clock;
    va_list arguments;
    va_start(arguments, format); vsnprintf(message, sizeof(message), format, arguments); va_end(arguments);
    clock_gettime(CLOCK_MONOTONIC, &clock);
    pthread_mutex_lock(&lock);
    int n = snprintf(line, sizeof(line), "REC seq=%llu t=%lld.%09ld %s\n",
                     (unsigned long long)++sequence, (long long)clock.tv_sec, clock.tv_nsec, message);
    size_t length = n > 0 && n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1;
    if (used + length > sizeof(pending) || bytes + used + length > PW_DIAGNOSTICS_CHUNK - 128u) flush();
    if (used + length <= sizeof(pending)) { memcpy(pending + used, line, length); used += length; }
    (void)ps5log_line(PS5LOG_INFO, message);
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_tick(uint64_t now)
{
    pthread_mutex_lock(&lock);
    if (now - last_flush >= 100000000ull) { flush(); last_flush = now; }
    if (!ps5log_enabled() && now - last_retry >= 5000000000ull) {
        (void)ps5log_reconnect(); last_retry = now;
    }
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_close(const char *reason)
{
    pw_diagnostics_log("PW_WINE64 session_end reason=%s", reason);
    pthread_mutex_lock(&lock);
    flush(); if (fd >= 0) { (void)fsync(fd); close(fd); fd = -1; }
    ps5log_close(reason);
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_resume(void)
{
    pthread_mutex_lock(&lock);
    if (fd < 0 && current[0]) {
        fd = open(current, O_WRONLY | O_APPEND);
        if (fd < 0) disk_error = errno;
    }
    (void)ps5log_reconnect();
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_status(char *buffer, size_t size)
{
    pthread_mutex_lock(&lock);
    snprintf(buffer, size, fd >= 0 ? "LOG SAVED - OPTIONS: REPORT HELP" : "LOG UNAVAILABLE (ERROR %d)", disk_error);
    pthread_mutex_unlock(&lock);
}

void pw_diagnostics_emergency(int signal, uintptr_t address, uint64_t pc)
{
    char line[128] = "PW_WINE64 fault signal=0x";
    size_t n = sizeof("PW_WINE64 fault signal=0x") - 1;
    const uint64_t values[3] = { (unsigned)signal, address, pc };
    const char *labels[3] = { " addr=0x", " rip=0x", "\n" };
    for (unsigned i = 0; i < 3; i++) {
        for (int shift = 60; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(values[i] >> shift) & 15];
        for (const char *p = labels[i]; *p; p++) line[n++] = *p;
    }
    if (fd >= 0) { ssize_t result = write(fd, line, n); (void)result; }
}
