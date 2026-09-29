/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it. */
#ifndef PW_WOW_HOST_MEMORY_H
#define PW_WOW_HOST_MEMORY_H
#include "../../include/prospero_win_vm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Where a guest thread's translator keeps its code and its cache entries:
 * memory Wine allocates for it, readable, writable and executable above the
 * guest's 4 GiB.
 *
 * With the translator's memory in Wine's own address space it lands where
 * Wine's anonymous memory does, which on the console is direct memory
 * (patch 0600) instead of the ~384 MiB that mmap(NULL) placements share.
 * One read-write-execute mapping also spares the translator a protection
 * change before and after every block it writes.
 *
 * On the console that memory is committed direct memory from its first
 * byte, and a thread's code arena (32 MiB, 128 MiB for the first thread) is
 * mostly never written: a game with a few dozen threads held gigabytes. A
 * region of lazy_bytes or more is therefore only reserved, and committed as
 * the translator writes into it: the engine protects the pages it is about
 * to write, and protect commits up to them, a step at a time. */
typedef struct PwWowHostMemory {
    /* Zeroed, readable, writable and executable memory of at least bytes,
     * aligned to `alignment`; NULL when there is none. */
    void *(*allocate)(size_t bytes);
    void (*release)(void *base, size_t bytes);
    size_t page;       /* protection granularity */
    size_t alignment;  /* what allocate aligns to */
    /* Optional, both or neither: address space for bytes, aligned as
     * allocate's and not yet usable, NULL when there is none; and commit,
     * which makes [base, +bytes) of it zeroed, readable, writable and
     * executable (0, or -1). Regions of lazy_bytes or more use them. */
    void *(*reserve)(size_t bytes);
    int (*commit)(void *base, size_t bytes);
    size_t lazy_bytes;
} PwWowHostMemory;

/* How much of a lazily committed region is committed, from its start: the
 * region's handle. */
typedef struct PwWowHostLazy {
    size_t committed;
} PwWowHostLazy;
#define PW_WOW_HOST_COMMIT_STEP ((size_t)1 << 20)

static inline int pw_wow_host_reserve(void *context, size_t bytes, size_t alignment, PwVmRegion *out)
{
    const PwWowHostMemory *memory = (const PwWowHostMemory *)context;
    size_t rounded;
    void *base;

    if (!out || !bytes || !alignment || (alignment & (alignment - 1)) || alignment > memory->alignment)
        return PW_ERR_PRECONDITION;
    if (bytes > SIZE_MAX - (memory->page - 1)) return PW_ERR_OVERFLOW;
    rounded = (bytes + memory->page - 1) & ~(memory->page - 1);
    out->handle = NULL;
    if (memory->reserve && memory->lazy_bytes && rounded >= memory->lazy_bytes)
    {
        PwWowHostLazy *lazy = (PwWowHostLazy *)calloc(1, sizeof(*lazy));

        if (!lazy) return PW_ERR_VM;
        if (!(base = memory->reserve(rounded)))
        {
            free(lazy);
            return PW_ERR_VM;
        }
        out->handle = lazy;
    }
    else if (!(base = memory->allocate(rounded))) return PW_ERR_VM;
    out->write_base = base;
    out->exec_base = base;
    out->bytes = rounded;
    out->alignment = memory->alignment;
    return PW_OK;
}

/* Committed and executable from the start, or, for a lazy region, as it is
 * written (protect): commit only checks the range. */
static inline int pw_wow_host_commit(void *context, const PwVmRegion *region, size_t offset,
                                     size_t bytes, unsigned protection)
{
    (void)context;
    (void)protection;
    return pw_vm_region_contains(region, offset, bytes) ? PW_OK : PW_ERR_PRECONDITION;
}

/* The mapping allows everything already; a lazy region is committed up to
 * the end of the range, rounded to a step. */
static inline int pw_wow_host_protect(void *context, const PwVmRegion *region, size_t offset,
                                      size_t bytes, unsigned protection)
{
    const PwWowHostMemory *memory = (const PwWowHostMemory *)context;
    PwWowHostLazy *lazy;
    size_t target;

    (void)protection;
    if (!pw_vm_region_contains(region, offset, bytes)) return PW_ERR_PRECONDITION;
    lazy = (PwWowHostLazy *)region->handle;
    if (!lazy || offset + bytes <= lazy->committed) return PW_OK;
    target = (offset + bytes + PW_WOW_HOST_COMMIT_STEP - 1) & ~(PW_WOW_HOST_COMMIT_STEP - 1);
    if (target > region->bytes) target = region->bytes;
    if (memory->commit((uint8_t *)region->write_base + lazy->committed, target - lazy->committed))
        return PW_ERR_VM;
    lazy->committed = target;
    return PW_OK;
}

static inline int pw_wow_host_release(void *context, PwVmRegion *region)
{
    const PwWowHostMemory *memory = (const PwWowHostMemory *)context;

    if (!region || !region->write_base) return PW_ERR_PRECONDITION;
    memory->release(region->write_base, region->bytes);
    free(region->handle);
    region->handle = NULL;
    region->write_base = NULL;
    region->exec_base = NULL;
    region->bytes = 0;
    return PW_OK;
}

static inline int pw_wow_host_backend(PwVmBackend *backend, PwWowHostMemory *memory)
{
    if (!backend || !memory || !memory->allocate || !memory->release || !memory->page ||
        (memory->page & (memory->page - 1)) || memory->alignment < memory->page ||
        !memory->reserve != !memory->commit)
        return PW_ERR_PRECONDITION;
    backend->context = memory;
    backend->capabilities = PW_VM_CAP_PROTECT;
    backend->page_bytes = memory->page;
    backend->reserve = pw_wow_host_reserve;
    backend->commit = pw_wow_host_commit;
    backend->protect = pw_wow_host_protect;
    backend->release = pw_wow_host_release;
    backend->reserve_at = NULL;
    return PW_OK;
}
#endif
