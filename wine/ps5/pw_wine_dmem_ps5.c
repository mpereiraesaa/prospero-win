/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The console side of pw_wine_dmem, linked into ntdll.prx: libkernel's
 * direct memory behind the mmap, munmap and mprotect calls of ntdll's
 * virtual memory manager (patch 0600 routes them here), for the address
 * ranges it reserves up front. Everything else passes straight through.
 *
 * The recipe is the one ps5-xash3d's heap runs on (FW 12.02): main direct
 * memory, mapped read-write at a fixed address over a reservation, then
 * protected with sceKernelMprotect; execute permission is refused at map
 * time but granted by mprotect. A self-check maps, writes, protects and
 * frees one page before the first region is taken; if any step fails, the
 * regions stay ordinary reservations and every call passes through. */
#include "pw_wine_dmem.h"
#include "pw_wine_heap.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/mman.h>

extern int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment);
extern int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags,
                                    int64_t offset, size_t alignment);
extern int sceKernelMprotect(const void *address, size_t bytes, int protection);
extern int sceKernelMunmap(void *address, size_t bytes);
extern int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes);

enum {
    PAGE = 0x4000,
    KERNEL_FIXED = 0x10,
    KERNEL_NO_OVERWRITE = 0x80,
    MEMORY_TYPE = 0x0c,       /* as ps5-xash3d's heap */
    CPU_READ_WRITE = 0x03,
    CPU_GPU_READ_WRITE = 0xf2,  /* what ps5-xash3d's heap maps with */
    RUNS = 32768,
};

/* WINE_PS5_WAIT_WATCHDOG: a direct-memory call slower than 50 ms is
 * logged (prospero-win#250). */
static uint64_t slow_start(void)
{
    static int enabled = -1;
    struct timespec ts;

    if (enabled == -1) enabled = getenv("WINE_PS5_WAIT_WATCHDOG") != NULL;
    if (!enabled) return 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
static void slow_end(uint64_t start, const char *what, size_t bytes)
{
    struct timespec ts;
    uint64_t now;

    if (!start) return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    if (now - start > 50000)
        fprintf(stderr, "wine-ps5: slow dmem %s bytes=%zu ms=%llu\n", what, bytes,
                (unsigned long long)((now - start) / 1000u));
}

/* CPU read-write, unless the kernel wants the GPU bits too. */
static int map_protection = CPU_READ_WRITE;

static int reserve(void *context, uintptr_t address, size_t bytes)
{
    void *at = (void *)address;
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelReserveVirtualRange(&at, bytes, KERNEL_FIXED | KERNEL_NO_OVERWRITE, PAGE) ||
         at != (void *)address;
    slow_end(t0, "reserve", bytes);
    return rc;
}
static int allocate(void *context, size_t bytes, int64_t *offset)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelAllocateMainDirectMemory(bytes, PAGE, MEMORY_TYPE, offset) != 0;
    slow_end(t0, "allocate", bytes);
    return rc;
}
static int map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    void *at = (void *)address;
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMapDirectMemory(&at, bytes, map_protection, KERNEL_FIXED, offset, PAGE);
    slow_end(t0, "map", bytes);
    if (rc && map_protection == CPU_READ_WRITE) {
        /* Refused CPU-only: the GPU bits, and keep them if they work. */
        at = (void *)address;
        rc = sceKernelMapDirectMemory(&at, bytes, CPU_GPU_READ_WRITE, KERNEL_FIXED, offset, PAGE);
        if (!rc) map_protection = CPU_GPU_READ_WRITE;
    }
    return rc || at != (void *)address;
}
static int protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMprotect((const void *)address, bytes, (int)protection) != 0;
    slow_end(t0, "protect", bytes);
    return rc;
}
static int unmap(void *context, uintptr_t address, size_t bytes)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelMunmap((void *)address, bytes) != 0;
    slow_end(t0, "unmap", bytes);
    return rc;
}
static int release(void *context, int64_t offset, size_t bytes)
{
    uint64_t t0 = slow_start();
    int rc;
    (void)context;
    rc = sceKernelReleaseDirectMemory(offset, bytes) != 0;
    slow_end(t0, "release", bytes);
    return rc;
}

static const PwWineDmemOps ops = { NULL, PAGE, reserve, allocate, map, protect, unmap, release };
static PwWineDmem dmem;
static PwWineDmemRun runs[RUNS];
/* 0 before the self-check, 1 in use, -1 refused: every call passes. */
static int state;

static uintptr_t page_down(const void *address) { return (uintptr_t)address & ~(uintptr_t)(PAGE - 1); }
static size_t page_span(const void *address, size_t bytes)
{
    return (((uintptr_t)address + bytes + PAGE - 1) & ~(uintptr_t)(PAGE - 1)) - page_down(address);
}

/* One page through every step, inside the first region, before any use. */
static int self_check(uintptr_t address)
{
    volatile uint8_t *bytes = (volatile uint8_t *)address;
    int ok = !pw_wine_dmem_replace(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE);

    if (ok) {
        bytes[0] = 0x5a;
        bytes[PAGE - 1] = 0xa5;
        ok = !pw_wine_dmem_protect(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_EXEC) &&
             bytes[0] == 0x5a && bytes[PAGE - 1] == 0xa5 &&
             !pw_wine_dmem_protect(&dmem, address, PAGE, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE);
    }
    return !pw_wine_dmem_replace(&dmem, address, PAGE, 0) && ok;
}

/* ntdll's reserved area [base, +size), already reserved: backed by direct
 * memory from now on. */
void __wine_ps5_dmem_region(void *base, size_t size)
{
    if (!state && pw_wine_dmem_init(&dmem, &ops, runs, RUNS, 1)) state = -1;
    if (state < 0 || pw_wine_dmem_add_region(&dmem, (uintptr_t)base, size, 1)) return;
    if (!state) {
        state = self_check((uintptr_t)base) ? 1 : -1;
        fprintf(stderr, "wine-ps5: direct memory %s for anonymous memory (region %p, %zu MiB)\n",
                state > 0 ? "in use" : "refused", base, size >> 20);
    }
}

void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0 || !(flags & MAP_FIXED) || (uintptr_t)address != at ||
        (!pw_wine_dmem_split(&dmem, at, end - at, &piece) && piece == end - at))
        return mmap(address, bytes, protection, flags, fd, offset);
    if (!(flags & MAP_ANON)) {
        void *mapped;

        /* A private file view (an image section) is read into direct
         * memory instead: ntdll falls back to pread when mmap reports
         * ENODEV. On the console a fixed file mapping over a reservation
         * does not give the requested pages. */
        if (!(flags & MAP_SHARED)) {
            errno = ENODEV;
            return MAP_FAILED;
        }
        /* Shared memory stays a file mapping, over what is there, as
         * before direct memory; only the exact address is accepted. */
        mapped = mmap(address, bytes, protection, flags, fd, offset);
        if (mapped == MAP_FAILED) return MAP_FAILED;
        if (mapped != address) {
            munmap(mapped, bytes);
            errno = ENOMEM;
            return MAP_FAILED;
        }
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) (void)pw_wine_dmem_adopt(&dmem, at, piece);
        return address;
    }
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, (unsigned)protection & 7);
        else if (mmap((void *)at, piece, protection, flags, -1, 0) == MAP_FAILED)
            status = -1;
    if (!status) return address;
    errno = ENOMEM;
    return MAP_FAILED;
}

/* Owned pages give their direct memory back, then their address space. */
int __wine_ps5_munmap(void *address, size_t bytes)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return munmap(address, bytes);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, 0) | sceKernelMunmap((void *)at, piece);
        else
            status |= munmap((void *)at, piece);
    if (status) errno = EINVAL;
    return status ? -1 : 0;
}

int __wine_ps5_mprotect(void *address, size_t bytes, int protection)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return mprotect(address, bytes, protection);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_protect(&dmem, at, piece, (unsigned)protection & 7);
        else
            status |= mprotect((void *)at, piece, protection);
    if (status) errno = ENOMEM;
    return status ? -1 : 0;
}

/* For the title's log, as __wine_virtual_stats: direct memory backed now and
 * at most, its runs and refused calls, then the heap's mapped and peak
 * mapped bytes. Returns how many counters there are. */
unsigned int __wine_ps5_memory_stats(uint64_t *out, unsigned int count)
{
    PwWineDmemStats dmem_stats = { 0 };
    PwWineHeapStats heap_stats;
    uint64_t values[6];

    if (state > 0) pw_wine_dmem_stats(&dmem, &dmem_stats);
    pw_wine_heap_stats(&heap_stats);
    values[0] = dmem_stats.backed_bytes;
    values[1] = dmem_stats.peak_backed_bytes;
    values[2] = dmem_stats.runs;
    values[3] = dmem_stats.failures;
    values[4] = heap_stats.mapped_bytes;
    values[5] = heap_stats.peak_mapped_bytes;
    for (unsigned int i = 0; i < count && i < 6; i++) out[i] = values[i];
    return 6;
}
