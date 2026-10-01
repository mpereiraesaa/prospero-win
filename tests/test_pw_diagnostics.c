/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#define PW_DIAGNOSTICS_TESTING 1
#include "../native/pw_diagnostics.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static _Atomic int online, retries, closed, slow_reconnect, slow_send;
static char raw[256];
static void pause_ms(unsigned ms) { struct timespec t = { 0, (long)ms * 1000000L }; nanosleep(&t, NULL); }
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
int ps5log_line(const char *level, const char *text) { (void)level; (void)text; if (slow_send) pause_ms(300); return 0; }
int ps5log_raw(const void *bytes, size_t length) { memcpy(raw, bytes, length < sizeof(raw) ? length : sizeof(raw) - 1); return 0; }
int ps5log_enabled(void) { return online; }
/* Like the real one, a reconnect to an absent server waits for the timeout. */
int ps5log_reconnect(void) { if (slow_reconnect) pause_ms(500); retries++; online = !slow_reconnect; return 0; }
void ps5log_close(const char *reason) { assert(reason); closed++; online = 0; }

static void *writer(void *arg)
{
    for (unsigned i = 0; i < 40; i++) pw_diagnostics_log("worker=%s item=%u", (char *)arg, i);
    return NULL;
}

int main(void)
{
    char root[] = "/tmp/pw-diagnostics-XXXXXX", path[512], data[8192], status[80];
    assert(mkdtemp(root));
    pw_diagnostics_test_retry_interval(20000000ull);
    online = 1;
    pw_diagnostics_log("before-open");
    assert(pw_diagnostics_open(root, "test-build", "pinball", 3) == 0);
    pw_diagnostics_status(status, sizeof(status)); assert(strstr(status, "LOG SAVED"));
    /* The title's loop never reconnects, whatever the channel's state. */
    online = 0; slow_reconnect = 1;
    uint64_t start = now_ms();
    for (unsigned i = 1; i <= 50; i++) pw_diagnostics_tick(6000000000ull * i);
    assert(now_ms() - start < 100);
    /* The retry thread does, and meanwhile records and flushes do not wait
     * for its connect timeout (they are saved; the network skips them). */
    while (!retries) pause_ms(5);
    start = now_ms();
    for (unsigned i = 0; i < 20; i++) { pw_diagnostics_log("during-reconnect=%u", i); pw_diagnostics_tick(400000000000ull + i * 200000000ull); }
    assert(now_ms() - start < 100);
    slow_reconnect = 0;
    while (!online) pause_ms(5);
    /* A slow send stalls only the thread that sends, never the file flush. */
    slow_send = 1;
    pthread_t sender; assert(!pthread_create(&sender, NULL, writer, "slow"));
    pause_ms(20);
    start = now_ms();
    for (unsigned i = 0; i < 10; i++) pw_diagnostics_tick(500000000000ull + i * 200000000ull);
    assert(now_ms() - start < 100);
    slow_send = 0; pthread_join(sender, NULL);
    /* A message longer than a record is cut, and says so. */
    char longest[3000]; memset(longest, 'x', sizeof(longest) - 1); longest[sizeof(longest) - 1] = 0;
    pw_diagnostics_log("%s", longest);
    pw_diagnostics_tick(600000000000ull);
    {
        int found = 0;
        for (unsigned part = 0; part < 2 && !found; part++) {
            snprintf(path, sizeof(path), part ? "%s/logs/session-0.previous.log" : "%s/logs/session-0.log", root);
            FILE *chunk = fopen(path, "r");
            if (!chunk) continue;
            size_t got = fread(data, 1, sizeof(data) - 1, chunk); data[got] = 0; fclose(chunk);
            char *at = strstr(data, " [truncated]\n");
            found = at && at - data > 1000 && at[-1] == 'x';
        }
        assert(found);
    }
    for (unsigned i = 0; i < 300; i++) pw_diagnostics_log("record=%u repeated information for bounded rotation", i);
    pw_diagnostics_tick(13000000000ull);
    snprintf(path, sizeof(path), "%s/logs/session-0.previous.log", root);
    struct stat st; assert(stat(path, &st) == 0 && st.st_size <= PW_DIAGNOSTICS_CHUNK);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, "a")); assert(!pthread_create(&b, NULL, writer, "b"));
    pthread_join(a, NULL); pthread_join(b, NULL);
    pw_diagnostics_tick(14000000000ull);
    pw_diagnostics_emergency(11, 0x1234, 0x5678, 1, 0x678);
    assert(strstr(raw, "fault signal=0x000000000000000b") && strstr(raw, "ntdll_segment=0x0000000000000001 offset=0x0000000000000678"));
    pw_diagnostics_close("wine-exit"); assert(closed == 1);
    snprintf(path, sizeof(path), "%s/logs/session-0.log", root);
    FILE *f = fopen(path, "r"); assert(f);
    size_t n = fread(data, 1, sizeof(data)-1, f); data[n] = 0; fclose(f);
    assert(strstr(data, "PW_REPORT/1 build=test-build profile=pinball cycle=3"));
    assert(strstr(data, "fault signal=0x000000000000000b") && strstr(data, "offset=0x0000000000000678"));
    assert(strstr(data, "session_end reason=wine-exit"));
    assert(stat(path, &st) == 0 && st.st_size <= PW_DIAGNOSTICS_CHUNK);
    pw_diagnostics_resume();
    pw_diagnostics_log("restart failed"); pw_diagnostics_close("restart-returned");
    f = fopen(path, "r"); assert(f); n = fread(data, 1, sizeof(data)-1, f); data[n] = 0; fclose(f);
    assert(strstr(data, "restart failed"));
    for (unsigned i = 0; i < 8; i++) {
        assert(pw_diagnostics_open(root, "next-build", "launcher", i) == 0);
        pw_diagnostics_close("launcher");
    }
    f = fopen(path, "r"); assert(f); n = fread(data, 1, sizeof(data)-1, f); data[n] = 0; fclose(f);
    assert(strstr(data, "build=next-build") && !strstr(data, "profile=pinball"));
    snprintf(path, sizeof(path), "%s/logs/session-0.previous.log", root); assert(access(path, F_OK) != 0);
    assert(pw_diagnostics_open("/dev/null", "build", "game", 0) == -1);
    pw_diagnostics_status(status, sizeof(status)); assert(strstr(status, "UNAVAILABLE"));
    for (unsigned i = 0; i < PW_DIAGNOSTICS_SESSIONS; i++) {
        snprintf(path, sizeof(path), "%s/logs/session-%u.log", root, i); unlink(path);
        snprintf(path, sizeof(path), "%s/logs/session-%u.previous.log", root, i); unlink(path);
    }
    snprintf(path, sizeof(path), "%s/logs/next.txt", root); unlink(path);
    snprintf(path, sizeof(path), "%s/logs", root); rmdir(path); rmdir(root);
    puts("diagnostics passed: bounded retention, concurrent writers, fault/end records, off-loop reconnects, no network waits in the loop, truncation marker, disk errors");
    return 0;
}
