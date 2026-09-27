/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it. */
#ifndef PW_WOW_HOST_MEMORY_H
#define PW_WOW_HOST_MEMORY_H
#include "../../include/prospero_win_vm.h"
#include <stddef.h>
#include <stdint.h>

/* Where a guest thread's translator keeps its code and its cache entries:
 * memory Wine allocates for it, committed readable, writable and executable
 * above the guest's 4 GiB.
 *
 * With the translator's memory in Wine's own address space it lands where
 * Wine's anonymous memory does, which on the console is direct memory
 * (patch 0600) instead of the ~384 MiB that mmap(NULL) placements share.
 * One read-write-execute mapping also spares the translator a protection
 * change before and after every block it writes: the backend's protect is
 * a no-op, since the mapping already allows both. */
typedef struct PwWowHostMemory {
    /* Zeroed, readable, writable and executable memory of at least bytes,
     * aligned to `alignment`; NULL when there is none. */
    void *(*allocate)(size_t bytes);
    void (*release)(void *base, size_t bytes);
    size_t page;       /* protection granularity */
    size_t alignment;  /* what allocate aligns to */
} PwWowHostMemory;

static inline int pw_wow_host_reserve(void *context, size_t bytes, size_t alignment, PwVmRegion *out)
{
    const PwWowHostMemory *memory = (const PwWowHostMemory *)context;
    size_t rounded;
    void *base;

    if (!out || !bytes || !alignment || (alignment & (alignment - 1)) || alignment > memory->alignment)
        return PW_ERR_PRECONDITION;
    if (bytes > SIZE_MAX - (memory->page - 1)) return PW_ERR_OVERFLOW;
    rounded = (bytes + memory->page - 1) & ~(memory->page - 1);
    if (!(base = memory->allocate(rounded))) return PW_ERR_VM;
    out->write_base = base;
    out->exec_base = base;
    out->bytes = rounded;
    out->alignment = memory->alignment;
    out->handle = NULL;
    return PW_OK;
}

/* Committed and executable from the start: only the range is checked. */
static inline int pw_wow_host_protect(void *context, const PwVmRegion *region, size_t offset,
                                      size_t bytes, unsigned protection)
{
    (void)context;
    (void)protection;
    return pw_vm_region_contains(region, offset, bytes) ? PW_OK : PW_ERR_PRECONDITION;
}

static inline int pw_wow_host_release(void *context, PwVmRegion *region)
{
    const PwWowHostMemory *memory = (const PwWowHostMemory *)context;

    if (!region || !region->write_base) return PW_ERR_PRECONDITION;
    memory->release(region->write_base, region->bytes);
    region->write_base = NULL;
    region->exec_base = NULL;
    region->bytes = 0;
    return PW_OK;
}

static inline int pw_wow_host_backend(PwVmBackend *backend, PwWowHostMemory *memory)
{
    if (!backend || !memory || !memory->allocate || !memory->release || !memory->page ||
        (memory->page & (memory->page - 1)) || memory->alignment < memory->page)
        return PW_ERR_PRECONDITION;
    backend->context = memory;
    backend->capabilities = PW_VM_CAP_PROTECT;
    backend->page_bytes = memory->page;
    backend->reserve = pw_wow_host_reserve;
    backend->commit = pw_wow_host_protect;
    backend->protect = pw_wow_host_protect;
    backend->release = pw_wow_host_release;
    backend->reserve_at = NULL;
    return PW_OK;
}
#endif
