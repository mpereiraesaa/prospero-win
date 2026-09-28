/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_DATA_MOUNT_H
#define PW_DATA_MOUNT_H
/*
 * Make /data available to this process.
 *
 * A title starts able to write only its own /download0 sandbox, where /data is
 * absent. prospero-win asks the Lapy JB daemon for it (see
 * docs/WINE_PS5_BUILD.md): in this process, before it creates any other
 * thread, seteuid(geteuid()) (which must succeed, or nothing is requested),
 * then a complete {"PID":<pid>} request renamed into /download0. The daemon
 * consuming it proves nothing, so prospero-win then waits until /data is
 * actually reachable.
 *
 * The file system and clock are injected so the request-and-wait logic is
 * host-testable; native/pw_data_mount.c provides the console operations. The
 * request is best-effort: without the helper the title keeps using /download0.
 */
#include <stddef.h>
#include <stdint.h>

#ifndef PW_DATA_MOUNT_REQUEST_PATH
#define PW_DATA_MOUNT_REQUEST_PATH "/download0/elevate_proc"
#endif
#ifndef PW_DATA_MOUNT_REQUEST_TEMP
/* Written first, then renamed to PW_DATA_MOUNT_REQUEST_PATH, so the daemon
 * only ever sees a complete request; the process id follows. */
#define PW_DATA_MOUNT_REQUEST_TEMP "/download0/.elevate_proc."
#endif
#ifndef PW_DATA_MOUNT_PATH
#define PW_DATA_MOUNT_PATH "/data"          /* reachable once granted */
#endif

#ifndef PW_DATA_MOUNT_WAIT_MS
#define PW_DATA_MOUNT_WAIT_MS 5000          /* how long to wait for /data */
#endif
#ifndef PW_DATA_MOUNT_POLL_MS
#define PW_DATA_MOUNT_POLL_MS 100           /* between checks */
#endif
#ifndef PW_DATA_MOUNT_SETTLE_MS
/* Once /data appears after a request, the helper may still be changing the
 * process: wait this long before the title goes on. All three console
 * power-offs of 2026-09-27 came within milliseconds of an escape. */
#define PW_DATA_MOUNT_SETTLE_MS 1000
#endif
enum { PW_DATA_MOUNT_REQUEST_MAX = 32 };    /* room for the request line */

typedef struct PwDataMountOps {
    /* Make this process eligible before requesting: seteuid(geteuid()) on
     * the console. 0 on success, -1 with errno set (nothing is requested). */
    int (*prepare)(void);
    /* Write len bytes as the request at path, complete or not at all. 0 on
     * success, -1 with errno set. */
    int (*write_request)(const char *path, const char *bytes, size_t len);
    /* Non-zero when PW_DATA_MOUNT_PATH is reachable. */
    int (*data_visible)(void);
    void (*sleep_ms)(int ms);
} PwDataMountOps;

typedef struct PwDataMountResult {
    int data_before;   /* /data was already reachable */
    int prepare_errno; /* errno if prepare failed (then nothing was requested), else 0 */
    int wrote_request; /* the request file was written */
    int write_errno;   /* errno if the write failed, else 0 */
    int data_after;    /* /data became reachable */
    int waited_ms;     /* time spent waiting for it */
    int settled_ms;    /* time waited after it appeared (a new grant only) */
} PwDataMountResult;   /* what one request reached, for logging */

/* Format the request line carrying pid into buf. Returns its length, or -1 if
 * buf is too small. */
int pw_data_mount_format_request(char *buf, size_t size, int32_t pid);

/* Request /data for pid, waiting up to max_wait_ms for it to appear. Returns 0
 * once /data is reachable (already, or after the request, then
 * PW_DATA_MOUNT_SETTLE_MS later), -1 otherwise.
 * detail, when non-NULL, is always filled. */
int pw_data_mount_request_with(const PwDataMountOps *ops, int32_t pid, int max_wait_ms,
                               PwDataMountResult *detail);

/* Request /data for this process with the native console operations. */
int pw_data_mount_request(PwDataMountResult *detail);

#endif
