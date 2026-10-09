/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_DMEM_H
#define PW_WINE_DMEM_H
#include <stddef.h>
#include <stdint.h>

/* Anonymous memory backed by the console's direct memory instead of its
 * flexible memory.
 *
 * A title has about 440 MiB of flexible memory, which every anonymous
 * mapping draws from, and up to 12 GiB of direct memory, which it allocates
 * by physical offset and maps where it chooses (measured on FW 12.02). This
 * module owns address regions, each reserved up front, and backs the pages
 * inside them with direct memory on demand. A page of a region is reserved
 * (nothing behind it: an access faults), backed by a run of direct memory
 * mapped read-write and then protected, or holds a mapping of the caller's
 * (a file). Freed pages return to reserved and their direct memory is
 * released at once.
 *
 * The kernel is reached only through PwWineDmemOps, so the policy is
 * portable and a host test stands in for the console. Every entry point
 * takes the module's own lock and none allocates from a heap: the table of
 * runs is caller storage. */

enum {
    PW_WINE_DMEM_READ = 1,
    PW_WINE_DMEM_WRITE = 2,
    PW_WINE_DMEM_EXEC = 4,
    PW_WINE_DMEM_MAX_REGIONS = 64,
};

/* What the module asks of the kernel. Addresses and sizes are multiples of
 * `page`. Each returns 0, or nonzero with nothing changed. */
typedef struct PwWineDmemOps {
    void *context;
    size_t page;
    /* Claim [address, +bytes) without memory behind it; refuse to replace
     * anything mapped there. */
    int (*reserve)(void *context, uintptr_t address, size_t bytes);
    /* Take `bytes` of direct memory; *offset is its physical offset. */
    int (*allocate)(void *context, size_t bytes, int64_t *offset);
    /* Map direct memory read-write at exactly address, over a reservation. */
    int (*map)(void *context, uintptr_t address, size_t bytes, int64_t offset);
    /* PW_WINE_DMEM_* protection of a mapped range. */
    int (*protect)(void *context, uintptr_t address, size_t bytes, unsigned protection);
    /* Remove whatever is mapped or reserved there. */
    int (*unmap)(void *context, uintptr_t address, size_t bytes);
    /* Give back direct memory, possibly a part of one allocation. */
    int (*release)(void *context, int64_t offset, size_t bytes);
} PwWineDmemOps;

/* Virtual pages mapped onto contiguous direct memory, or, with offset
 * PW_WINE_DMEM_CALLER, a mapping of the caller's. */
#define PW_WINE_DMEM_CALLER ((int64_t)-1)
typedef struct PwWineDmemRun {
    uintptr_t address;
    size_t bytes;
    int64_t offset;
} PwWineDmemRun;

typedef struct PwWineDmemStats {
    uint64_t backed_bytes, peak_backed_bytes;
    /* Of those, below 4 GiB: the part an i386 guest's address space holds. */
    uint64_t low_backed_bytes, peak_low_backed_bytes;
    uint64_t allocations, releases, maps, protects, failures;
    uint32_t runs, peak_runs;
} PwWineDmemStats;

typedef struct PwWineDmem {
    PwWineDmemOps ops;
    int lock;
    uintptr_t region_low[PW_WINE_DMEM_MAX_REGIONS], region_high[PW_WINE_DMEM_MAX_REGIONS];
    unsigned regions;
    PwWineDmemRun *runs;       /* sorted by address, never overlapping */
    uint32_t run_count, run_capacity;
    unsigned zero_fill;
    PwWineDmemStats stats;
} PwWineDmem;

/* runs is storage for capacity entries. zero_fill clears newly backed
 * pages, for a kernel that hands out direct memory uncleared. 0, or -1. */
int pw_wine_dmem_init(PwWineDmem *, const PwWineDmemOps *, PwWineDmemRun *runs,
                      uint32_t capacity, unsigned zero_fill);
/* Own [address, +bytes) from now on, reserving it first unless the caller
 * already has. A region touching or overlapping one already owned joins
 * it. 0, or -1. */
int pw_wine_dmem_add_region(PwWineDmem *, uintptr_t address, size_t bytes, int reserved);
/* Stop owning every region that lies wholly inside [address, +bytes) and
 * holds no run: its address space is gone. Returns how many were dropped. */
int pw_wine_dmem_remove_regions(PwWineDmem *, uintptr_t address, size_t bytes);
/* 1 when [address, +bytes) lies inside one region. */
int pw_wine_dmem_owns(PwWineDmem *, uintptr_t address, size_t bytes);
/* Where [address, +bytes) first crosses a region's edge: *piece is the
 * length of its leading part, which is wholly inside a region (1) or wholly
 * outside every region (0). A range touching both is handled piece by
 * piece. */
int pw_wine_dmem_split(PwWineDmem *, uintptr_t address, size_t bytes, size_t *piece);

/* Fresh zeroed memory with this protection, replacing whatever the range
 * held, backed or the caller's; protection 0 leaves it reserved. This is
 * an anonymous MAP_FIXED mapping. 0, or -1 with the range reserved. */
int pw_wine_dmem_replace(PwWineDmem *, uintptr_t address, size_t bytes, unsigned protection);
/* The caller has mapped something of its own (a file) over the range with
 * a fixed mapping: the direct memory it replaced is released, and the
 * range is protected like the rest but never backed. 0, or -1. */
int pw_wine_dmem_adopt(PwWineDmem *, uintptr_t address, size_t bytes);
/* Direct memory the caller already holds, at offset, mapped over the range
 * with this protection (0 maps it inaccessible), replacing whatever the
 * range held. Several ranges may map the same direct memory (a shared
 * section's views); the module never releases it. 0, or -1 with the range
 * reserved. */
int pw_wine_dmem_map_shared(PwWineDmem *, uintptr_t address, size_t bytes, int64_t offset,
                            unsigned protection);
/* Commit: the backed pages and the caller's mappings keep their contents
 * and take the protection; reserved pages are backed, zeroed, first,
 * unless protection is 0. 0, or -1. */
int pw_wine_dmem_protect(PwWineDmem *, uintptr_t address, size_t bytes, unsigned protection);
/* How many bytes of the range are backed by direct memory. */
size_t pw_wine_dmem_backed(PwWineDmem *, uintptr_t address, size_t bytes);
void pw_wine_dmem_stats(PwWineDmem *, PwWineDmemStats *);

#endif
