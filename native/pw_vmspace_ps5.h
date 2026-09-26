/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VMSPACE_PS5_H
#define PW_VMSPACE_PS5_H
#include <stdint.h>

enum { PW_VMSPACE_RANGES = 16 };

typedef struct PwVmSpaceRange { uint64_t base, bytes; } PwVmSpaceRange;

typedef struct PwVmSpaceReport {
    long page_size;                  /* sysconf(_SC_PAGESIZE) */
    uint64_t low_free_bytes;         /* free 16 MiB slots between 16 MiB and 4 GiB */
    uint32_t range_count;
    PwVmSpaceRange ranges[PW_VMSPACE_RANGES];
    uint64_t largest_run_base, largest_run_bytes;
    int largest_single_ok;           /* the largest run reserved in one mapping */
    int granularity_64k_ok, granularity_4k_ok; /* fixed reservations at those alignments */
    int first_errno;
} PwVmSpaceReport;

int pw_vmspace_ps5_probe(PwVmSpaceReport *report);
#endif
