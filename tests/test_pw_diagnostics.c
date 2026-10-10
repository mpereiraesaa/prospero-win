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

static _Atomic int online, retries, closed, slow_reconnect, slow_send_ms, sending, sent;
static char raw[256], last_sent[256];
static void pause_ms(unsigned ms) { struct timespec t = { ms / 1000u, (long)(ms % 1000u) * 1000000L }; nanosleep(&t, NULL); }
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
int ps5log_line(const char *level, const char *text)
{
    (void)level;
    /* The writer sends the message, not the saved REC line around it. */
    assert(strncmp(text, "REC ", 4) != 0);
    snprintf(last_sent, sizeof(last_sent), "%s", text);
    sent++;
    if (slow_send_ms) { sending = 1; pause_ms((unsigned)slow_send_ms); sending = 0; }
    return 0;
}
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

static char data[1 << 16];

/* The session's chunk (part 0) or its previous chunk (part 1), as text. */
static int read_chunk(const char *root, unsigned part)
{
    char path[512];
    snprintf(path, sizeof(path), part ? "%s/logs/session-0.previous.log" : "%s/logs/session-0.log", root);
    FILE *chunk = fopen(path, "r");
    if (!chunk) { data[0] = 0; return 0; }
    size_t got = fread(data, 1, sizeof(data) - 1, chunk); data[got] = 0; fclose(chunk);
    return 1;
}

static char *session_has(const char *root, const char *text)
{
    for (unsigned part = 0; part < 2; part++) {
        if (!read_chunk(root, part)) continue;
        char *at = strstr(data, text);
        if (at) return at;
    }
    return NULL;
}

/* The sequence number of the record holding `at`, a position in data. */
static unsigned long long seq_of(char *at)
{
    unsigned long long seq = 0;
    assert(at);
    while (at > data && at[-1] != '\n') at--;
    assert(sscanf(at, "REC seq=%llu", &seq) == 1);
    return seq;
}

/* How long a record takes to reach the file with nobody asking for it. */
static uint64_t wait_saved(const char *root, const char *text, unsigned limit_ms)
{
    uint64_t start = now_ms();
    while (!session_has(root, text)) { assert(now_ms() - start < limit_ms); pause_ms(2); }
    return now_ms() - start;
}

int main(void)
{
    char root[] = "/tmp/pw-diagnostics-XXXXXX", path[512], status[80];
    assert(mkdtemp(root));
    pw_diagnostics_test_retry_interval(20000000ull);
    online = 1;
    /* When the writer thread cannot start, close still saves and sends the
     * records: the session is just not written between the title's calls. */
    pw_diagnostics_test_refuse_writer(1);
    pw_diagnostics_log("no-writer-record");
    assert(pw_diagnostics_open(root, "test-build", "pinball", 2) == 0);
    pw_diagnostics_log("no-writer-record-2");
    pw_diagnostics_close("no-writer");
    assert(closed == 1 && sent == 3 && !strcmp(last_sent, "PW_WINE64 session_end reason=no-writer"));
    assert(read_chunk(root, 0) && strstr(data, "no-writer-record\n") && strstr(data, "no-writer-record-2\n") && strstr(data, "session_end reason=no-writer"));
    pw_diagnostics_test_refuse_writer(0);
    online = 1; closed = 0; sent = 0;
    snprintf(path, sizeof(path), "%s/logs/session-0.log", root); unlink(path);
    snprintf(path, sizeof(path), "%s/logs/next.txt", root); unlink(path);
    pw_diagnostics_log("before-open");
    assert(pw_diagnostics_open(root, "test-build", "pinball", 3) == 0);
    pw_diagnostics_status(status, sizeof(status)); assert(!strcmp(status, "LOG SAVED - OPTIONS: REPORT HELP"));
    /* A record from before the session is saved into it, and sent, by the
     * writer: the thread that logged it did neither. */
    pw_diagnostics_test_settle();
    assert(session_has(root, "REC seq=4 t=") && strstr(data, "before-open\n"));
    assert(sent == 1 && !strcmp(last_sent, "before-open"));
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
    assert(wait_saved(root, "during-reconnect=19", 400) < 400);
    slow_reconnect = 0;
    while (!online) pause_ms(5);
    pause_ms(5); /* the retry thread clears its flag after the channel is up */
    /* A slow send stalls only the writer's network pass: the thread that
     * logs and the title's loop go on, and the file is written. */
    slow_send_ms = 300;
    pthread_t sender; assert(!pthread_create(&sender, NULL, writer, "slow"));
    start = now_ms();
    pthread_join(sender, NULL);
    for (unsigned i = 0; i < 10; i++) pw_diagnostics_tick(500000000000ull + i * 200000000ull);
    assert(now_ms() - start < 100);
    assert(wait_saved(root, "worker=slow item=39", 1000) < 1000);
    slow_send_ms = 0;
    pw_diagnostics_test_settle();
    assert(!strcmp(last_sent, "worker=slow item=39"));
    /* Without a tick, the writer's own cadence saves a record within 100 ms. */
    pw_diagnostics_log("cadence-check");
    assert(wait_saved(root, "cadence-check", 1000) < 300);
    /* A message longer than a record is cut, and says so. */
    char longest[3000]; memset(longest, 'x', sizeof(longest) - 1); longest[sizeof(longest) - 1] = 0;
    pw_diagnostics_log("%s", longest);
    pw_diagnostics_tick(600000000000ull);
    pw_diagnostics_test_settle();
    {
        char *at = session_has(root, " [truncated]\n");
        assert(at && at - data > 1000 && at[-1] == 'x');
    }
    for (unsigned i = 0; i < 300; i++) pw_diagnostics_log("record=%u repeated information for bounded rotation", i);
    pw_diagnostics_tick(13000000000ull);
    pw_diagnostics_test_settle();
    snprintf(path, sizeof(path), "%s/logs/session-0.previous.log", root);
    struct stat st; assert(stat(path, &st) == 0 && st.st_size <= PW_DIAGNOSTICS_CHUNK);
    snprintf(path, sizeof(path), "%s/logs/session-0.log", root);
    assert(stat(path, &st) == 0 && st.st_size <= PW_DIAGNOSTICS_CHUNK);
    /* Every chunk starts with the identity line, records stay whole across
     * the rotation, and the newest are in the current chunk. */
    assert(read_chunk(root, 0) && !strncmp(data, "PW_REPORT/1 build=test-build", 28) && strstr(data, "record=299 repeated"));
    assert(read_chunk(root, 1) && !strncmp(data, "PW_REPORT/1 build=test-build", 28) && data[strlen(data) - 1] == '\n');
    pw_diagnostics_status(status, sizeof(status)); assert(!strcmp(status, "LOG SAVED - OPTIONS: REPORT HELP"));
    /* A writer that cannot keep up (here: one send that takes a second)
     * fills the buffer; the records past that are dropped, never waited for,
     * and the writer says how many once it is through. */
    slow_send_ms = 1000;
    pw_diagnostics_log("stall");
    pw_diagnostics_tick(20000000000ull);
    while (!sending) pause_ms(1);
    uint64_t logged = 0;
    start = now_ms();
    while (logged < PW_DIAGNOSTICS_PENDING / 20) pw_diagnostics_log("burst=%llu", (unsigned long long)logged++);
    assert(now_ms() - start < 500);
    pw_diagnostics_status(status, sizeof(status));
    unsigned long long shown = 0;
    assert(sscanf(status, "LOG SAVED (%llu DROPPED) - OPTIONS: REPORT HELP", &shown) == 1 && shown > 0 && shown < logged);
    slow_send_ms = 0;
    pw_diagnostics_test_settle();
    pw_diagnostics_log("after-burst");
    pw_diagnostics_test_settle();
    {
        unsigned long long dropped = 0, dropped_bytes = 0;
        char text[48];
        char *notice = session_has(root, "PW_WINE64 diagnostics dropped records=");
        assert(notice && sscanf(notice, "PW_WINE64 diagnostics dropped records=%llu bytes=%llu", &dropped, &dropped_bytes) == 2);
        assert(dropped == shown && dropped_bytes > dropped * 20);
        /* Exactly the last records of the burst are missing, the sequence
         * numbers count them, and the record after the notice follows it. */
        snprintf(text, sizeof(text), "burst=%llu\n", logged - dropped - 1);
        char *last = session_has(root, text); assert(last);
        unsigned long long seq_last = seq_of(last);
        snprintf(text, sizeof(text), "burst=%llu\n", logged - dropped);
        assert(!session_has(root, text));
        unsigned long long seq_drop = seq_of(session_has(root, "PW_WINE64 diagnostics dropped records="));
        assert(seq_drop == seq_last + dropped + 1);
        assert(seq_of(session_has(root, "after-burst")) == seq_drop + 1);
        assert(!strcmp(last_sent, "after-burst"));
    }
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, "a")); assert(!pthread_create(&b, NULL, writer, "b"));
    pthread_join(a, NULL); pthread_join(b, NULL);
    pw_diagnostics_tick(14000000000ull);
    pw_diagnostics_test_settle();
    assert(session_has(root, "worker=a item=39") && session_has(root, "worker=b item=39"));
    pw_diagnostics_emergency(11, 0x1234, 0x5678, 1, 0x678);
    assert(strstr(raw, "fault signal=0x000000000000000b") && strstr(raw, "ntdll_segment=0x0000000000000001 offset=0x0000000000000678"));
    pw_diagnostics_close("wine-exit"); assert(closed == 1);
    assert(!strcmp(last_sent, "PW_WINE64 session_end reason=wine-exit"));
    /* The fault line is in the chunk that was current when it struck; the
     * end record may have rotated into a new one. */
    assert(session_has(root, "fault signal=0x000000000000000b") && strstr(data, "offset=0x0000000000000678"));
    assert(read_chunk(root, 0));
    assert(strstr(data, "PW_REPORT/1 build=test-build profile=pinball cycle=3"));
    assert(strstr(data, "session_end reason=wine-exit"));
    assert(stat(path, &st) == 0 && st.st_size <= PW_DIAGNOSTICS_CHUNK);
    pw_diagnostics_resume();
    pw_diagnostics_log("restart failed"); pw_diagnostics_close("restart-returned");
    assert(read_chunk(root, 0) && strstr(data, "restart failed"));
    for (unsigned i = 0; i < 8; i++) {
        assert(pw_diagnostics_open(root, "next-build", "launcher", i) == 0);
        pw_diagnostics_close("launcher");
    }
    assert(read_chunk(root, 0) && strstr(data, "build=next-build") && !strstr(data, "profile=pinball"));
    snprintf(path, sizeof(path), "%s/logs/session-0.previous.log", root); assert(access(path, F_OK) != 0);
    assert(pw_diagnostics_open("/dev/null", "build", "game", 0) == -1);
    pw_diagnostics_status(status, sizeof(status)); assert(strstr(status, "UNAVAILABLE"));
    /* Without a file, records still reach the network through the writer. */
    while (!online) pause_ms(5);
    pause_ms(5);
    int sent_before = sent;
    pw_diagnostics_log("no-file");
    pw_diagnostics_test_settle();
    assert(sent == sent_before + 1 && !strcmp(last_sent, "no-file"));
    for (unsigned i = 0; i < PW_DIAGNOSTICS_SESSIONS; i++) {
        snprintf(path, sizeof(path), "%s/logs/session-%u.log", root, i); unlink(path);
        snprintf(path, sizeof(path), "%s/logs/session-%u.previous.log", root, i); unlink(path);
    }
    snprintf(path, sizeof(path), "%s/logs/next.txt", root); unlink(path);
    snprintf(path, sizeof(path), "%s/logs", root); rmdir(path); rmdir(root);
    puts("diagnostics passed: bounded retention, writer thread, concurrent writers, fault/end records, off-loop reconnects, no file or network waits in callers, dropped records counted, truncation marker, disk errors");
    return 0;
}
