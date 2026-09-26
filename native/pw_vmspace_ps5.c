/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Address-space facts for Wine's virtual memory manager on the console:
 * the host page size, which low (< 4 GiB) ranges can be reserved at a fixed
 * address, the largest single low reservation, and whether a fixed hint is
 * honoured at 64 KiB (Windows allocation) granularity.
 */
#include "pw_vmspace_ps5.h"
#include "../include/prospero_win.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_EXCL
#define MAP_EXCL 0x00004000   /* FreeBSD: fail instead of replacing a mapping */
#endif

enum { STEP = 16u << 20, LOW_END_MB = 4096 };

static void *try_fixed(uint64_t address, size_t bytes)
{
    void *got = mmap((void *)(uintptr_t)address, bytes, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_EXCL, -1, 0);
    return got == MAP_FAILED ? NULL : got;
}

int pw_vmspace_ps5_probe(PwVmSpaceReport *r)
{
    uint64_t run_start = 0, run_bytes = 0;

    if (!r) return PW_ERR_PRECONDITION;
    memset(r, 0, sizeof(*r));
    r->page_size = sysconf(_SC_PAGESIZE);

    /* Walk the low 4 GiB in 16 MiB slots, reserving each free slot and
     * releasing it immediately; coalesce adjacent free slots into ranges. */
    for (uint64_t mb = 16; mb < LOW_END_MB; mb += STEP >> 20) {
        const uint64_t address = mb << 20;
        void *got = try_fixed(address, STEP);

        if (got) {
            (void)munmap(got, STEP);
            r->low_free_bytes += STEP;
            if (run_bytes == 0) run_start = address;
            run_bytes += STEP;
            continue;
        }
        if (errno != 0 && r->first_errno == 0) r->first_errno = errno;
        if (run_bytes && r->range_count < PW_VMSPACE_RANGES) {
            r->ranges[r->range_count].base = run_start;
            r->ranges[r->range_count].bytes = run_bytes;
            r->range_count++;
        }
        if (run_bytes > r->largest_run_bytes) {
            r->largest_run_bytes = run_bytes;
            r->largest_run_base = run_start;
        }
        run_bytes = 0;
    }
    if (run_bytes) {
        if (r->range_count < PW_VMSPACE_RANGES) {
            r->ranges[r->range_count].base = run_start;
            r->ranges[r->range_count].bytes = run_bytes;
            r->range_count++;
        }
        if (run_bytes > r->largest_run_bytes) {
            r->largest_run_bytes = run_bytes;
            r->largest_run_base = run_start;
        }
    }

    /* One reservation spanning the largest free run: can it be held whole? */
    if (r->largest_run_bytes) {
        void *got = try_fixed(r->largest_run_base, (size_t)r->largest_run_bytes);
        r->largest_single_ok = got != NULL;
        if (got) (void)munmap(got, (size_t)r->largest_run_bytes);
    }

    /* A 64 KiB-aligned fixed reservation that is not page-size aligned for a
     * 16 KiB host would still be accepted; record what the kernel returns. */
    if (r->largest_run_bytes) {
        const uint64_t address = r->largest_run_base + 0x10000;
        void *got = try_fixed(address, 0x10000);
        r->granularity_64k_ok = got == (void *)(uintptr_t)address;
        if (got) (void)munmap(got, 0x10000);
        got = try_fixed(r->largest_run_base + 0x1000, 0x1000);
        r->granularity_4k_ok = got == (void *)(uintptr_t)(r->largest_run_base + 0x1000);
        if (got) (void)munmap(got, 0x1000);
    }
    return PW_OK;
}
