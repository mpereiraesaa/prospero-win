/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The native console operations of pw_data_mount, exercised against the real
 * file system. The Makefile compiles this with the request and data paths
 * pointed at a temporary location and a short wait, so open/write/stat and the
 * poll wrapper run without a PS5. */
#include "../native/pw_data_mount.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void cleanup(void)
{
    unlink(PW_DATA_MOUNT_REQUEST_PATH);
    rmdir(PW_DATA_MOUNT_PATH);
}

int main(void)
{
    PwDataMountResult r;
    char want[PW_DATA_MOUNT_REQUEST_MAX], got[64];
    int fd;
    ssize_t n;

    cleanup();

    /* /data absent: the request file is written and the wait times out. */
    assert(pw_data_mount_request(&r) == -1);
    assert(r.data_before == 0 && r.wrote_request == 1 && r.write_errno == 0);
    assert(r.data_after == 0 && r.waited_ms == PW_DATA_MOUNT_WAIT_MS);

    /* It holds this process's id in the line the helper reads. */
    assert((fd = open(PW_DATA_MOUNT_REQUEST_PATH, O_RDONLY)) >= 0);
    n = read(fd, got, sizeof(got) - 1);
    close(fd);
    assert(n > 0);
    got[n] = 0;
    assert(pw_data_mount_format_request(want, sizeof(want), (int)getpid()) > 0);
    assert(!strcmp(got, want));

    /* /data present: reported reachable up front, no new request written. */
    assert(mkdir(PW_DATA_MOUNT_PATH, 0755) == 0);
    unlink(PW_DATA_MOUNT_REQUEST_PATH);
    assert(pw_data_mount_request(&r) == 0);
    assert(r.data_before == 1 && r.data_after == 1 && r.wrote_request == 0);
    assert(access(PW_DATA_MOUNT_REQUEST_PATH, F_OK) != 0); /* nothing written */

    cleanup();
    printf("data mount native passed: request file written, pid line, /data present short-circuit\n");
    return 0;
}
