/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Host build models the console helper as unavailable and checks fallback. */
#include "../native/pw_data_mount.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    PwDataMountResult r;
    rmdir(PW_DATA_MOUNT_PATH);

    assert(pw_data_mount_request(&r) == -1);
    assert(r.data_before == 0 && r.helper_completed == 0);
    assert(r.helper_errno == ENOENT && r.data_after == 0);

    assert(mkdir(PW_DATA_MOUNT_PATH, 0755) == 0);
    assert(pw_data_mount_request(&r) == 0);
    assert(r.data_before == 1 && r.data_after == 1 && r.helper_completed == 0);
    assert(r.helper_errno == 0);
    rmdir(PW_DATA_MOUNT_PATH);
    puts("data mount native: missing helper falls back; existing /data skips request");
    return 0;
}
