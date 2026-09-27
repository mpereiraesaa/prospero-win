/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_DATA_MOUNT_H
#define PW_DATA_MOUNT_H
/*
 * Make /data available to this process.
 *
 * A title starts able to write only its own /download0 sandbox, where /data is
 * absent. prospero-win writes a small request file into that sandbox carrying
 * its process id; an external console helper watches for the file and makes
 * /data available to the process (see docs/WINE_PS5_BUILD.md for the helper it
 * expects). prospero-win then waits until /data is reachable.
 *
 * The file system and clock are injected so the request-and-wait logic is
 * host-testable; native/pw_data_mount.c provides the console operations. The
 * request is best-effort: without the helper the title keeps using /download0.
 */
#include <stddef.h>
#include <stdint.h>

#ifndef PW_DATA_MOUNT_REQUEST_PATH
#define PW_DATA_MOUNT_REQUEST_PATH "/download0/etahen_jailbreak"
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
    /* Write len bytes to path (truncating). 0 on success, -1 with errno set. */
    int (*write_request)(const char *path, const char *bytes, size_t len);
    /* Non-zero when PW_DATA_MOUNT_PATH is reachable. */
    int (*data_visible)(void);
    void (*sleep_ms)(int ms);
} PwDataMountOps;

typedef struct PwDataMountResult {
    int data_before;   /* /data was already reachable */
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
