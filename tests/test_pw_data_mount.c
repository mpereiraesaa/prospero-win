/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The portable half of the /data mount request: the request line and the
 * write-then-wait-for-/data logic, driven by injected operations. The real
 * file system and clock are exercised only on the console. */
#include "../native/pw_data_mount.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char written_path[128];
static char written_bytes[64];
static int write_calls, write_fail, visible_in, poll_calls, sleep_total;

static int fake_write(const char *path, const char *bytes, size_t len)
{
    write_calls++;
    snprintf(written_path, sizeof(written_path), "%s", path);
    if (len < sizeof(written_bytes)) { memcpy(written_bytes, bytes, len); written_bytes[len] = 0; }
    if (write_fail) { errno = EROFS; return -1; }
    return 0;
}

/* /data appears after visible_in more checks (models the helper acting). */
static int fake_visible(void)
{
    poll_calls++;
    if (visible_in > 0) { visible_in--; return 0; }
    return 1;
}

static void fake_sleep(int ms) { sleep_total += ms; }

int main(void)
{
    static const PwDataMountOps ops = { fake_write, fake_visible, fake_sleep };
    PwDataMountResult r;
    char buf[PW_DATA_MOUNT_REQUEST_MAX], small[3];

    /* The request line carries the process id. */
    assert(pw_data_mount_format_request(buf, sizeof(buf), 2595) == 14 && !strcmp(buf, "{\"PID\":\"2595\"}"));
    assert(pw_data_mount_format_request(buf, sizeof(buf), 1) == 11 && !strcmp(buf, "{\"PID\":\"1\"}"));
    assert(pw_data_mount_format_request(small, sizeof(small), 2595) == -1); /* too small */

    /* Request: write the file, then /data appears after a couple of checks. */
    write_calls = poll_calls = sleep_total = 0; write_fail = 0; visible_in = 3;
    assert(pw_data_mount_request_with(&ops, 2595, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(r.data_before == 0 && r.wrote_request == 1 && r.write_errno == 0);
    /* one check before the write, two sleeps, visible on the third loop check */
    assert(r.data_after == 1 && r.waited_ms == 2 * PW_DATA_MOUNT_POLL_MS);
    /* a new grant waits to settle before the title goes on */
    assert(r.settled_ms == PW_DATA_MOUNT_SETTLE_MS && sleep_total == r.waited_ms + r.settled_ms);
    assert(!strcmp(written_path, PW_DATA_MOUNT_REQUEST_PATH) && !strcmp(written_bytes, "{\"PID\":\"2595\"}"));
    assert(write_calls == 1);

    /* Already reachable: no request written, no waiting. */
    write_calls = poll_calls = sleep_total = 0; visible_in = 0;
    assert(pw_data_mount_request_with(&ops, 2595, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(r.data_before == 1 && r.data_after == 1 && r.wrote_request == 0 && write_calls == 0);
    assert(r.settled_ms == 0 && sleep_total == 0);

    /* Never granted: fail after the deadline, request still recorded. */
    write_calls = poll_calls = sleep_total = 0; visible_in = 1000000;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(r.wrote_request == 1 && r.data_after == 0 && r.waited_ms == 300 && r.settled_ms == 0);

    /* Write failure is reported but the wait still runs. */
    write_calls = poll_calls = sleep_total = 0; write_fail = 1; visible_in = 2;
    assert(pw_data_mount_request_with(&ops, 7, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(r.wrote_request == 0 && r.write_errno != 0 && r.data_after == 1);

    /* Missing ops are rejected. */
    assert(pw_data_mount_request_with(NULL, 7, 100, &r) == -1);

    printf("data mount passed: request line, wait, already-present, timeout, write failure, settle after a grant\n");
    return 0;
}
