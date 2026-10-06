/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_DATA_MOUNT_H
#define PW_DATA_MOUNT_H
/* Ask the bundled Lapy ELF helper to make /data available to this process. */
#include <stddef.h>
#include <stdint.h>

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
typedef struct PwDataMountOps {
    /* Complete the helper request/prepare/result exchange for pid. */
    int (*request)(int32_t pid);
    /* Non-zero when PW_DATA_MOUNT_PATH is reachable. */
    int (*data_visible)(void);
    void (*sleep_ms)(int ms);
    /* Non-zero when this process is elevated, not only shown /data by
     * another payload; NULL treats a visible /data as an elevated one. */
    int (*elevated)(void);
} PwDataMountOps;

typedef struct PwDataMountResult {
    int data_before;   /* /data was already reachable */
    int helper_completed; /* helper returned success for this process */
    int helper_errno;     /* errno for a failed request, else 0 */
    int data_after;    /* /data became reachable */
    int waited_ms;     /* time spent waiting for it */
    int settled_ms;    /* time waited after it appeared (a new grant only) */
} PwDataMountResult;   /* what one request reached, for logging */

/* Request /data for pid, waiting up to max_wait_ms after a successful helper
 * response. Returns 0 once access is visible and settled, or -1 on failure.
 * detail, when non-NULL, is always filled. */
int pw_data_mount_request_with(const PwDataMountOps *ops, int32_t pid, int max_wait_ms,
                               PwDataMountResult *detail);

/* Request /data for this process with the native console operations. */
int pw_data_mount_request(PwDataMountResult *detail);

#endif
