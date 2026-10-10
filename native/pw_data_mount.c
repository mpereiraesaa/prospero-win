/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_data_mount.h"
#include "pw_lapy_elevation.h"

#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int pw_data_mount_request_with(const PwDataMountOps *ops, int32_t pid,
                               int max_wait_ms, PwDataMountResult *detail)
{
    PwDataMountResult local;

    if (!detail) detail = &local;
    detail->data_before = detail->helper_completed = detail->helper_errno = 0;
    detail->data_after = detail->waited_ms = detail->settled_ms = 0;
    detail->elevated_before = detail->elevated_after = 0;
    if (!ops || !ops->request || !ops->data_visible || !ops->sleep_ms ||
        pid <= 1 || max_wait_ms < 0) return -1;

    /* A new LoadExec process starts fresh, but an existing grant is reusable.
     * Another payload (PS5SXHelper's mountroot, for one) can show /data to
     * every app without elevating it, and Wine then fails in
     * server_init_process: ask the helper anyway. */
    if (ops->data_visible()) {
        detail->data_before = 1;
        if (!ops->elevated || ops->elevated()) {
            detail->elevated_before = detail->elevated_after = 1;
            detail->data_after = 1;
            return 0;
        }
    }

    errno = 0;
    if (ops->request(pid) == 0)
        detail->helper_completed = 1;
    else
        detail->helper_errno = errno ? errno : EIO;

    /* A successful wire response is necessary, then confirm actual access.
     * Without one, /data that was already visible is still usable as before. */
    if (!detail->helper_completed) {
        if (!detail->data_before) return -1;
        detail->data_after = 1;
        return 0;
    }
    for (;;) {
        /* On a /data that was already visible only the title's own elevation
         * is news, so that is what the helper's reply has to bring about. */
        if (detail->data_before ? (!ops->elevated || ops->elevated()) : ops->data_visible()) {
            detail->elevated_after = !ops->elevated || ops->elevated();
            detail->data_after = 1;
            ops->sleep_ms(PW_DATA_MOUNT_SETTLE_MS);
            detail->settled_ms = PW_DATA_MOUNT_SETTLE_MS;
            return 0;
        }
        if (detail->waited_ms >= max_wait_ms) {
            /* The helper said yes but the title was not elevated within the
             * deadline: keep the visible /data as before the request, and
             * leave data_after and elevated_after at 0 for the log. */
            return detail->data_before ? 0 : -1;
        }
        ops->sleep_ms(PW_DATA_MOUNT_POLL_MS);
        detail->waited_ms += PW_DATA_MOUNT_POLL_MS;
    }
}

static int native_helper_request(int32_t pid)
{
#if defined(PW_DATA_MOUNT_HOST_TEST)
    (void)pid;
    errno = ENOENT;
    return -1;
#else
    return pw_lapy_elevation_request(pid);
#endif
}

static int native_data_visible(void)
{
    struct stat st;
    return stat(PW_DATA_MOUNT_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

/* An unelevated title gets EPERM from lstat() on every path while stat()
 * still works (prospero-win #376), so only that error says "unelevated". */
static int native_elevated(void)
{
    struct stat st;
    return lstat(PW_DATA_MOUNT_PATH, &st) == 0 || errno != EPERM;
}

static void native_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&ts, NULL);
}

int pw_data_mount_request(PwDataMountResult *detail)
{
    static const PwDataMountOps ops = {
        native_helper_request, native_data_visible, native_sleep_ms, native_elevated};
    return pw_data_mount_request_with(&ops, (int32_t)getpid(),
                                      PW_DATA_MOUNT_WAIT_MS, detail);
}
