/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "pw_data_mount.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int pw_data_mount_format_request(char *buf, size_t size, int32_t pid)
{
    /* The Lapy JB daemon reads the process id from {"PID":<int>}. */
    int len = snprintf(buf, size, "{\"PID\":%d}\n", (int)pid);
    if (len < 0 || (size_t)len >= size) return -1;
    return len;
}

int pw_data_mount_request_with(const PwDataMountOps *ops, int32_t pid, int max_wait_ms,
                               PwDataMountResult *detail)
{
    PwDataMountResult local;
    char request[PW_DATA_MOUNT_REQUEST_MAX];
    int len;

    if (!detail) detail = &local;
    detail->data_before = detail->wrote_request = detail->write_errno = 0;
    detail->prepare_errno = detail->data_after = detail->waited_ms = detail->settled_ms = 0;
    if (!ops || !ops->prepare || !ops->write_request || !ops->data_visible || !ops->sleep_ms) return -1;

    /* Already reachable (e.g. a re-launch): nothing to request. */
    if (ops->data_visible()) { detail->data_before = detail->data_after = 1; return 0; }

    /* The daemon only acts on a process that did this first. */
    errno = 0;
    if (ops->prepare() != 0) {
        detail->prepare_errno = errno ? errno : EPERM;
        return -1;
    }

    if ((len = pw_data_mount_format_request(request, sizeof(request), pid)) < 0) return -1;
    errno = 0;
    if (ops->write_request(PW_DATA_MOUNT_REQUEST_PATH, request, (size_t)len) == 0)
        detail->wrote_request = 1;
    else
        detail->write_errno = errno;

    for (;;) {
        if (ops->data_visible()) {
            detail->data_after = 1;
            ops->sleep_ms(PW_DATA_MOUNT_SETTLE_MS);
            detail->settled_ms = PW_DATA_MOUNT_SETTLE_MS;
            return 0;
        }
        if (detail->waited_ms >= max_wait_ms) return -1;
        ops->sleep_ms(PW_DATA_MOUNT_POLL_MS);
        detail->waited_ms += PW_DATA_MOUNT_POLL_MS;
    }
}

/* ---- native console operations ----------------------------------------- */

static int native_prepare(void)
{
    /* The same-UID call gives this process a private credential, which the
     * daemon requires; it must come before any other thread exists. */
    return seteuid(geteuid());
}

static int native_write_request(const char *path, const char *bytes, size_t len)
{
    char temporary[64];
    ssize_t put;
    int fd, saved;

    if (snprintf(temporary, sizeof(temporary), "%s%ld", PW_DATA_MOUNT_REQUEST_TEMP, (long)getpid()) >=
        (int)sizeof(temporary)) {
        errno = EOVERFLOW;
        return -1;
    }
    (void)unlink(temporary);
    if ((fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) return -1;
    put = write(fd, bytes, len);
    saved = put == (ssize_t)len ? 0 : put >= 0 ? EIO : errno;
    if (close(fd) != 0 && !saved) saved = errno;
    /* The daemon sees only a complete request. */
    if (!saved && rename(temporary, path) != 0) saved = errno;
    if (saved) { (void)unlink(temporary); errno = saved; return -1; }
    return 0;
}

static int native_data_visible(void)
{
    struct stat st;
    return stat(PW_DATA_MOUNT_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

static void native_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    (void)nanosleep(&ts, NULL);
}

int pw_data_mount_request(PwDataMountResult *detail)
{
    static const PwDataMountOps ops = {
        native_prepare, native_write_request, native_data_visible, native_sleep_ms };
    return pw_data_mount_request_with(&ops, (int32_t)getpid(), PW_DATA_MOUNT_WAIT_MS, detail);
}
