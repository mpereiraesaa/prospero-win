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
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

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
 * memory from now on. ntdll also gives each reservation it makes at a fixed
 * address outside those areas: the console does not back a bare reservation
 * when it is protected, so committing one would leave it without memory.
 * Such a region is dropped again when it is unmapped. */
void __wine_ps5_dmem_region(void *base, size_t size)
{
    if (!state && pw_wine_dmem_init(&dmem, &ops, runs, RUNS, 1)) state = -1;
    if (state < 0) return;
    if (pw_wine_dmem_add_region(&dmem, (uintptr_t)base, size, 1)) {
        fprintf(stderr, "wine-ps5: no direct memory region for %p, %zu bytes\n", base, size);
        return;
    }
    if (!state) {
        state = self_check((uintptr_t)base) ? 1 : -1;
        fprintf(stderr, "wine-ps5: direct memory %s for anonymous memory (region %p, %zu MiB)\n",
                state > 0 ? "in use" : "refused", base, size >> 20);
    }
}

/* Anonymous sections in direct memory.
 *
 * Wine backs an anonymous section (NtCreateSection without a file) with a
 * POSIX shared memory object, and on the console every view of one is drawn
 * from the title's ~440 MiB of flexible memory. A browser creates hundreds:
 * Battle.net's login page ran flexible memory down to nothing and its 2 MiB
 * views failed with ENOMEM while gigabytes of direct memory sat unused. The
 * in-process server registers each such section here (patch 0871): it gets
 * its own direct memory, zeroed, and every view of it maps that memory, so
 * all views show the same bytes. The memory goes back once the section
 * object is gone and its last view is unmapped. A section larger than
 * SECTION_LIMIT, or any when the console refuses to map one block of direct
 * memory twice, keeps its shared memory object.
 *
 * A view of a registered section never falls back to the object: it is not
 * written again once the section has its direct memory, so such a view
 * would show zeros while the others show the contents, and nothing would
 * fail. A view inside the regions goes through the module (a run it never
 * releases); one outside them, one straddling a region's edge or one the
 * kernel places maps the same direct memory straight from the kernel; a
 * private view gets a copy, the module's inside the regions and ordinary
 * anonymous memory outside; and what cannot be mapped fails with ENOMEM.
 * The views are kept in a table scanned only up to its highest used slot. */
#ifndef PW_WINE_SECTION_VIEWS
#define PW_WINE_SECTION_VIEWS 8192     /* the host test shrinks it to fill it */
#endif
enum { SECTIONS = 4096, VIEWS = PW_WINE_SECTION_VIEWS };
#define SECTION_LIMIT ((size_t)256 << 20)
typedef struct {
    uint64_t dev, ino;
    int64_t offset;     /* its direct memory */
    size_t bytes;
    uint32_t refs;      /* the section object's, and one per view; 0 free */
} Section;
typedef struct {
    uintptr_t low, high;
    uint32_t section;   /* index + 1; 0 free */
} View;
static Section sections[SECTIONS];
static View views[VIEWS];
static uint32_t view_top;   /* every slot from here up is free */
static int section_lock, section_count;
/* 0 untested, 1 one block maps twice, -1 refused */
static int alias_state;
static uint64_t section_bytes, section_peak;
static int views_full_logged;

static void sections_lock(void)
{
    while (__atomic_exchange_n(&section_lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&section_lock, __ATOMIC_RELAXED)) {}
}
static void sections_unlock(void) { __atomic_store_n(&section_lock, 0, __ATOMIC_RELEASE); }

/* Caller holds the lock. */
static void section_unref(uint32_t index)
{
    Section *section = &sections[index];

    if (!section->refs || --section->refs) return;
    (void)sceKernelReleaseDirectMemory(section->offset, section->bytes);
    section_bytes -= section->bytes;
    section_count--;
    memset(section, 0, sizeof(*section));
}

/* Caller holds the lock: a free view slot, or -1 when the table is full. */
static int view_slot(void)
{
    uint32_t i;

    for (i = 0; i < view_top && views[i].section; i++) {}
    if (i == VIEWS) return -1;
    if (i == view_top) view_top++;
    return (int)i;
}

/* Caller holds the lock: the top slot and the free ones below it go. */
static void view_trim(void)
{
    while (view_top && !views[view_top - 1].section) view_top--;
}

/* One page of direct memory mapped at two addresses the kernel chooses:
 * a write through one must show through the other. */
static int alias_check(void)
{
    void *first = NULL, *second = NULL;
    int64_t offset;
    int ok = 0;

    if (sceKernelAllocateMainDirectMemory(PAGE, PAGE, MEMORY_TYPE, &offset)) return 0;
    if (!sceKernelMapDirectMemory(&first, PAGE, map_protection, 0, offset, PAGE)) {
        if (!sceKernelMapDirectMemory(&second, PAGE, map_protection, 0, offset, PAGE)) {
            ((volatile uint8_t *)first)[7] = 0x5a;
            ok = second != first && ((volatile uint8_t *)second)[7] == 0x5a;
            (void)sceKernelMunmap(second, PAGE);
        }
        (void)sceKernelMunmap(first, PAGE);
    }
    (void)sceKernelReleaseDirectMemory(offset, PAGE);
    return ok;
}

/* The in-process server's: a cookie for a new anonymous section of this
 * size whose shared memory object is fd, or 0 to keep that object. */
int __wine_ps5_section_create(int fd, unsigned long long size)
{
    size_t bytes = (size_t)((size + PAGE - 1) & ~(unsigned long long)(PAGE - 1));
    void *scratch = NULL;
    struct stat st;
    int64_t offset;
    uint32_t i;

    if (state <= 0 || !bytes || size > SECTION_LIMIT) return 0;
    if (!alias_state) {
        alias_state = alias_check() ? 1 : -1;
        fprintf(stderr, "wine-ps5: anonymous sections in direct memory: %s\n",
                alias_state > 0 ? "on" : "off (a block does not map twice)");
    }
    if (alias_state < 0 || fstat(fd, &st)) return 0;
    if (sceKernelAllocateMainDirectMemory(bytes, PAGE, MEMORY_TYPE, &offset)) return 0;
    /* Direct memory comes uncleared; a section starts zeroed. */
    if (sceKernelMapDirectMemory(&scratch, bytes, map_protection, 0, offset, PAGE)) {
        (void)sceKernelReleaseDirectMemory(offset, bytes);
        return 0;
    }
    memset(scratch, 0, bytes);
    (void)sceKernelMunmap(scratch, bytes);
    sections_lock();
    for (i = 0; i < SECTIONS && sections[i].refs; i++) {}
    if (i < SECTIONS) {
        sections[i] = (Section){ (uint64_t)st.st_dev, (uint64_t)st.st_ino, offset, bytes, 1 };
        section_count++;
        section_bytes += bytes;
        if (section_bytes > section_peak) section_peak = section_bytes;
    }
    sections_unlock();
    if (i == SECTIONS) {
        (void)sceKernelReleaseDirectMemory(offset, bytes);
        return 0;
    }
    return (int)i + 1;
}

/* The server's section object is gone; its views keep the memory. */
void __wine_ps5_section_release(int cookie)
{
    if (cookie <= 0 || cookie > SECTIONS) return;
    sections_lock();
    section_unref((uint32_t)cookie - 1);
    sections_unlock();
}

/* [low, high) is being replaced or unmapped: the views there end. */
static void drop_views(uintptr_t low, uintptr_t high)
{
    if (!__atomic_load_n(&section_count, __ATOMIC_RELAXED)) return;
    sections_lock();
    for (uint32_t i = 0; i < view_top; i++) {
        View *view = &views[i];
        if (!view->section || view->high <= low || view->low >= high) continue;
        if (low <= view->low && high >= view->high) {
            section_unref(view->section - 1);
            view->section = 0;
        } else if (low <= view->low) {
            view->low = high;
        } else if (high >= view->high) {
            view->high = low;
        } else {
            /* A hole in the middle: the right part is a view of its own when
             * there is room; otherwise this one spans the hole until it
             * goes. */
            int j = view_slot();
            if (j >= 0) {
                views[j] = (View){ high, view->high, view->section };
                sections[view->section - 1].refs++;
                view->high = low;
            }
        }
    }
    view_trim();
    sections_unlock();
}

/* The registered section behind fd, with a reference taken, or -1. */
static int find_section(int fd)
{
    struct stat st;
    uint32_t i;

    if (!__atomic_load_n(&section_count, __ATOMIC_RELAXED) || fd < 0 || fstat(fd, &st)) return -1;
    sections_lock();
    for (i = 0; i < SECTIONS; i++)
        if (sections[i].refs && sections[i].ino == (uint64_t)st.st_ino && sections[i].dev == (uint64_t)st.st_dev) {
            sections[i].refs++;
            break;
        }
    sections_unlock();
    return i < SECTIONS ? (int)i : -1;
}

static void unref_section(uint32_t i)
{
    sections_lock();
    section_unref(i);
    sections_unlock();
}

/* Direct memory from `from` mapped where the kernel chooses (or at the hint
 * `at`, which it may ignore), read-write. 0 with *at set, or -1. */
static int map_anywhere(void **at, size_t bytes, int64_t from)
{
    void *where = *at;
    int rc = sceKernelMapDirectMemory(&where, bytes, map_protection, 0, from, PAGE);

    if (rc && map_protection == CPU_READ_WRITE) {
        where = *at;
        rc = sceKernelMapDirectMemory(&where, bytes, CPU_GPU_READ_WRITE, 0, from, PAGE);
        if (!rc) map_protection = CPU_GPU_READ_WRITE;
    }
    if (rc) return -1;
    *at = where;
    return 0;
}

/* [at, end) of a view goes back to what a fixed mapping leaves: reserved
 * inside the regions, nothing outside. */
static void undo_pieces(uintptr_t at, uintptr_t end)
{
    size_t piece;

    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) (void)pw_wine_dmem_replace(&dmem, at, piece, 0);
        else (void)unmap(NULL, at, piece);
}

/* A section's direct memory from `from` mapped at the fixed address [at,
 * end) with this protection, piece by piece along the regions' edges:
 * through the module inside them, straight from the kernel outside. 0, or
 * -1 with the range undone. */
static int map_pieces(uintptr_t at, uintptr_t end, int64_t from, unsigned protection)
{
    uintptr_t start = at;
    size_t piece;

    for (; at < end; at += piece, from += (int64_t)piece) {
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) {
            if (pw_wine_dmem_map_shared(&dmem, at, piece, from, protection)) break;
        } else {
            if (map(NULL, at, piece, from)) break;
            if (protection != (PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE) && protect(NULL, at, piece, protection)) {
                (void)unmap(NULL, at, piece);
                break;
            }
        }
    }
    if (at >= end) return 0;
    undo_pieces(start, at);
    return -1;
}

/* Where a view of registered section i, `bytes` from `offset`, may be. */
static int view_fits(uint32_t i, size_t bytes, off_t offset)
{
    return offset >= 0 && !(offset & (PAGE - 1)) && (size_t)offset <= sections[i].bytes &&
           bytes <= sections[i].bytes - (size_t)offset;
}

/* A shared view of registered section i: its direct memory at the address
 * asked, or where the kernel puts it. The address, or MAP_FAILED with
 * errno. */
static void *map_registered(void *address, size_t bytes, int protection, int flags, uint32_t i, off_t offset)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    unsigned prot = (unsigned)protection & 7;
    int64_t from = sections[i].offset + offset;
    int slot;

    if (!view_fits(i, end - at, offset) || ((flags & MAP_FIXED) && (uintptr_t)address != at)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    sections_lock();
    if ((slot = view_slot()) < 0) {
        sections_unlock();
        if (!views_full_logged++) fprintf(stderr, "wine-ps5: no room for another section view (%u)\n", VIEWS);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    views[slot] = (View){ at, at, i + 1 };   /* claimed, empty until mapped */
    sections[i].refs++;
    sections_unlock();
    if (flags & MAP_FIXED) {
        drop_views(at, end);
        if (map_pieces(at, end, from, prot)) goto failed;
    } else {
        void *where = address;
        if (map_anywhere(&where, end - at, from)) goto failed;
        if (prot != (PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE) && protect(NULL, (uintptr_t)where, end - at, prot)) {
            (void)unmap(NULL, (uintptr_t)where, end - at);
            goto failed;
        }
        address = where;
        at = (uintptr_t)where;
        end = at + page_span(address, bytes);
    }
    sections_lock();
    views[slot] = (View){ at, end, i + 1 };
    sections_unlock();
    return address;
failed:
    sections_lock();
    views[slot].section = 0;
    section_unref(i);
    view_trim();
    sections_unlock();
    errno = ENOMEM;
    return MAP_FAILED;
}

/* A private (copy-on-write) view of registered section i: fresh memory
 * holding a copy of its bytes, the module's inside the regions, ordinary
 * anonymous memory outside them or where the kernel puts it. ntdll would
 * otherwise read the shared memory object, which a registered section
 * never writes. The address, or MAP_FAILED with errno. */
static void *copy_registered(void *address, size_t bytes, int protection, int flags, uint32_t i, off_t offset)
{
    const unsigned read_write = PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE;
    uintptr_t at = page_down(address), end = at + page_span(address, bytes), p;
    unsigned prot = (unsigned)protection & 7;
    size_t len = end - at, piece;
    void *scratch = NULL;
    int failed = 0;

    if (!view_fits(i, len, offset) || ((flags & MAP_FIXED) && (uintptr_t)address != at) ||
        map_anywhere(&scratch, len, sections[i].offset + offset)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (!(flags & MAP_FIXED)) {
        void *where = mmap(address, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (where != MAP_FAILED) {
            memcpy(where, scratch, len);
            if (prot != read_write && mprotect(where, len, protection)) {
                (void)munmap(where, len);
                where = MAP_FAILED;
            }
        }
        (void)sceKernelMunmap(scratch, len);
        if (where == MAP_FAILED) errno = ENOMEM;
        return where;
    }
    drop_views(at, end);
    for (p = at; p < end && !failed; p += piece) {
        int inside = pw_wine_dmem_split(&dmem, p, end - p, &piece);
        if (inside ? pw_wine_dmem_replace(&dmem, p, piece, read_write)
                   : mmap((void *)p, piece, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
            break;
        memcpy((void *)p, (const char *)scratch + (p - at), piece);
        if (prot != read_write &&
            (inside ? pw_wine_dmem_protect(&dmem, p, piece, prot) : mprotect((void *)p, piece, protection)))
            failed = 1;     /* this piece is mapped: undone with the rest */
    }
    (void)sceKernelMunmap(scratch, len);
    if (p >= end && !failed) return address;
    undo_pieces(at, p);
    errno = ENOMEM;
    return MAP_FAILED;
}

void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0, section;

    if (state <= 0) return mmap(address, bytes, protection, flags, fd, offset);
    /* A registered section's view maps its direct memory, never the shared
     * memory object, wherever it goes. */
    if (!(flags & MAP_ANON) && (section = find_section(fd)) >= 0) {
        void *result = (flags & MAP_SHARED)
            ? map_registered(address, bytes, protection, flags, (uint32_t)section, offset)
            : copy_registered(address, bytes, protection, flags, (uint32_t)section, offset);
        unref_section((uint32_t)section);
        return result;
    }
    if (!(flags & MAP_FIXED) || (uintptr_t)address != at ||
        (!pw_wine_dmem_split(&dmem, at, end - at, &piece) && piece == end - at)) {
        void *mapped = mmap(address, bytes, protection, flags, fd, offset);
        /* A fixed mapping replaced whatever was there, section views too. */
        if (mapped != MAP_FAILED && (flags & MAP_FIXED) && (uintptr_t)address == at) drop_views(at, end);
        return mapped;
    }
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
        drop_views(at, end);
        for (; at < end; at += piece)
            if (pw_wine_dmem_split(&dmem, at, end - at, &piece)) (void)pw_wine_dmem_adopt(&dmem, at, piece);
        return address;
    }
    drop_views(at, end);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, (unsigned)protection & 7);
        else if (mmap((void *)at, piece, protection, flags, -1, 0) == MAP_FAILED)
            status = -1;
    if (!status) return address;
    errno = ENOMEM;
    return MAP_FAILED;
}

/* Owned pages give their direct memory back, then their address space; the
 * views there end first, so a region holding one is never dropped while a
 * view still points into it (a view is a run of the module's). */
int __wine_ps5_munmap(void *address, size_t bytes)
{
    uintptr_t at = page_down(address), end = at + page_span(address, bytes);
    size_t piece;
    int status = 0;

    if (state <= 0) return munmap(address, bytes);
    drop_views(at, end);
    for (; at < end; at += piece)
        if (pw_wine_dmem_split(&dmem, at, end - at, &piece))
            status |= pw_wine_dmem_replace(&dmem, at, piece, 0) | sceKernelMunmap((void *)at, piece);
        else
            status |= munmap((void *)at, piece);
    pw_wine_dmem_remove_regions(&dmem, page_down(address), page_span(address, bytes));
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
 * at most, its runs and refused calls, the heap's mapped and peak mapped
 * bytes, the direct memory backed below 4 GiB now and at most, then the
 * anonymous sections' direct memory now and at most. Returns
 * how many counters there are. */
unsigned int __wine_ps5_memory_stats(uint64_t *out, unsigned int count)
{
    PwWineDmemStats dmem_stats = { 0 };
    PwWineHeapStats heap_stats;
    uint64_t values[10];

    if (state > 0) pw_wine_dmem_stats(&dmem, &dmem_stats);
    pw_wine_heap_stats(&heap_stats);
    values[0] = dmem_stats.backed_bytes;
    values[1] = dmem_stats.peak_backed_bytes;
    values[2] = dmem_stats.runs;
    values[3] = dmem_stats.failures;
    values[4] = heap_stats.mapped_bytes;
    values[5] = heap_stats.peak_mapped_bytes;
    values[6] = dmem_stats.low_backed_bytes;
    values[7] = dmem_stats.peak_low_backed_bytes;
    sections_lock();
    values[8] = section_bytes;
    values[9] = section_peak;
    sections_unlock();
    for (unsigned int i = 0; i < count && i < 10; i++) out[i] = values[i];
    return 10;
}
