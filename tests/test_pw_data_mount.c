/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host-side checks for helper completion, /data visibility and settle timing. */
#include "../native/pw_data_mount.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

static int request_calls, request_fail, visible_in, poll_calls, sleep_total;
static int32_t requested_pid;

static int fake_request(int32_t pid)
{
    request_calls++;
    requested_pid = pid;
    if (request_fail) { errno = ETIMEDOUT; return -1; }
    return 0;
}

static int fake_visible(void)
{
    poll_calls++;
    if (visible_in > 0) { visible_in--; return 0; }
    return 1;
}

static void fake_sleep(int ms) { sleep_total += ms; }

int main(void)
{
    static const PwDataMountOps ops = {fake_request, fake_visible, fake_sleep};
    PwDataMountResult r;

    request_calls = poll_calls = sleep_total = 0;
    request_fail = 0;
    visible_in = 3;
    assert(pw_data_mount_request_with(&ops, 2595, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(request_calls == 1 && requested_pid == 2595);
    assert(r.data_before == 0 && r.helper_completed == 1 && r.helper_errno == 0);
    assert(r.data_after == 1 && r.waited_ms == 2 * PW_DATA_MOUNT_POLL_MS);
    assert(r.settled_ms == PW_DATA_MOUNT_SETTLE_MS);
    assert(sleep_total == r.waited_ms + r.settled_ms);

    /* Already reachable: no helper request or delay. */
    request_calls = poll_calls = sleep_total = 0;
    visible_in = 0;
    assert(pw_data_mount_request_with(&ops, 2595, PW_DATA_MOUNT_WAIT_MS, &r) == 0);
    assert(r.data_before == 1 && r.data_after == 1 && request_calls == 0);
    assert(sleep_total == 0);

    /* A completed helper that fails to expose /data times out. */
    request_calls = poll_calls = sleep_total = 0;
    visible_in = 1000000;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(request_calls == 1 && r.helper_completed == 1);
    assert(r.data_after == 0 && r.waited_ms == 300 && r.settled_ms == 0);

    /* No completion response means no access and no settle delay. */
    request_calls = poll_calls = sleep_total = 0;
    request_fail = 1;
    visible_in = 1000000;
    assert(pw_data_mount_request_with(&ops, 7, 300, &r) == -1);
    assert(r.helper_completed == 0 && r.helper_errno == ETIMEDOUT);
    assert(r.data_after == 0 && sleep_total == 0);
    request_fail = 0;

    assert(pw_data_mount_request_with(NULL, 7, 100, &r) == -1);
    assert(pw_data_mount_request_with(&ops, 1, 100, &r) == -1);
    puts("data mount: helper result, access visibility, deadline and settle delay covered");
    return 0;
}
