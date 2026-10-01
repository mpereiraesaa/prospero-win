/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "../native/pw_diagnostics.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int online, retries, closed;
int ps5log_line(const char *level, const char *text) { (void)level; (void)text; return 0; }
int ps5log_enabled(void) { return online; }
int ps5log_reconnect(void) { retries++; online = 1; return 0; }
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
    pw_diagnostics_log("before-open");
    assert(pw_diagnostics_open(root, "test-build", "pinball", 3) == 0);
    pw_diagnostics_status(status, sizeof(status)); assert(strstr(status, "LOG SAVED"));
    pw_diagnostics_tick(6000000000ull); assert(retries == 1);
    online = 0;
    pw_diagnostics_tick(7000000000ull); assert(retries == 1);
    pw_diagnostics_tick(12000000000ull); assert(retries == 2);
    for (unsigned i = 0; i < 300; i++) pw_diagnostics_log("record=%u repeated information for bounded rotation", i);
    pw_diagnostics_tick(13000000000ull);
    snprintf(path, sizeof(path), "%s/logs/session-0.previous.log", root);
    struct stat st; assert(stat(path, &st) == 0 && st.st_size <= PW_DIAGNOSTICS_CHUNK);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, "a")); assert(!pthread_create(&b, NULL, writer, "b"));
    pthread_join(a, NULL); pthread_join(b, NULL);
    pw_diagnostics_tick(14000000000ull);
    pw_diagnostics_emergency(11, 0x1234, 0x5678);
    pw_diagnostics_close("wine-exit"); assert(closed == 1);
    snprintf(path, sizeof(path), "%s/logs/session-0.log", root);
    FILE *f = fopen(path, "r"); assert(f);
    size_t n = fread(data, 1, sizeof(data)-1, f); data[n] = 0; fclose(f);
    assert(strstr(data, "PW_REPORT/1 build=test-build profile=pinball cycle=3"));
    assert(strstr(data, "fault signal=0x000000000000000b"));
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
    puts("diagnostics passed: bounded retention, concurrent writers, fault/end records, retry cadence, disk errors");
    return 0;
}
