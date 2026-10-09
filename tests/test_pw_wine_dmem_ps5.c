/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The console glue of pw_wine_dmem with libkernel replaced by a host model:
 * direct memory is a memfd, a reservation is a PROT_NONE mapping that
 * refuses to overlap, and, as on the console, execute permission is
 * refused at map time. Checks what ntdll's calls do inside and outside the
 * regions, and that a console refusing the recipe leaves every call to the
 * host.
 */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_dmem.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

void __wine_ps5_dmem_region(void *base, size_t size);
void *__wine_ps5_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset);
int __wine_ps5_munmap(void *address, size_t bytes);
int __wine_ps5_mprotect(void *address, size_t bytes, int protection);
unsigned int __wine_ps5_memory_stats(uint64_t *out, unsigned int count);
int __wine_ps5_section_create(int fd, unsigned long long size);
void __wine_ps5_section_release(int cookie);

enum { PAGE = 0x4000, PHYS_PAGES = 256 };
static int phys_fd, refuse_map, cpu_only_refused, refuse_alias;
static unsigned char allocated[PHYS_PAGES];

int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment)
{
    void *at;
    (void)alignment;
    assert(flags == 0x90);
    at = mmap(*address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (at == MAP_FAILED) return (int)0x8002000c;
    return 0;
}
int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset)
{
    unsigned count = (unsigned)(bytes / PAGE), run = 0;
    assert(alignment == PAGE && type == 0x0c);
    for (unsigned p = 0; p < PHYS_PAGES; p++) {
        run = allocated[p] ? 0 : run + 1;
        if (run == count) {
            memset(&allocated[p + 1 - count], 1, count);
            *offset = (int64_t)(p + 1 - count) * PAGE;
            return 0;
        }
    }
    return (int)0x8002000c;
}
int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset,
                             size_t alignment)
{
    (void)alignment;
    if (refuse_map || (protection & 4)) return (int)0x80020016;
    /* A kernel that wants the GPU bits refuses CPU-only memory. */
    if (cpu_only_refused && protection == 3) return (int)0x80020016;
    assert(protection == 3 || protection == 0xf2);
    if (!flags) {
        /* The kernel chooses the address; a console that refuses to map one
         * block twice refuses the second. */
        void *at;
        if (refuse_alias) {
            static int mapped_once;
            if (mapped_once++ & 1) return (int)0x80020016;
        }
        at = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, phys_fd, offset);
        if (at == MAP_FAILED) return (int)0x8002000c;
        *address = at;
        return 0;
    }
    assert(flags == 0x10);
    return mmap(*address, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, phys_fd, offset) ==
           MAP_FAILED;
}
int sceKernelMprotect(const void *address, size_t bytes, int protection)
{
    return mprotect((void *)address, bytes, protection);
}
int sceKernelMunmap(void *address, size_t bytes) { return munmap(address, bytes); }
int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes)
{
    for (size_t b = 0; b < bytes; b += PAGE) {
        assert(allocated[(offset + (int64_t)b) / PAGE]);
        allocated[(offset + (int64_t)b) / PAGE] = 0;
    }
    return 0;
}

/* A read through one view of what was written through another: the
 * compiler must not assume the two addresses are unrelated. */
#define SHOWS(pointer, index) (((volatile uint8_t *)(pointer))[index])

static unsigned used(void)
{
    unsigned count = 0;
    for (unsigned p = 0; p < PHYS_PAGES; p++) count += allocated[p];
    return count;
}
/* Nothing at all is mapped at [address, +bytes). */
static int free_space(uint8_t *address, size_t bytes)
{
    void *probe = mmap(address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (probe == MAP_FAILED) return 0;
    munmap(probe, bytes);
    return 1;
}

static uint8_t *space_for(size_t pages)
{
    uint8_t *raw = mmap(NULL, (pages + 1) * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *aligned;
    assert(raw != MAP_FAILED);
    aligned = (uint8_t *)(((uintptr_t)raw + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    munmap(raw, (pages + 1) * PAGE);
    return aligned;
}
/* Pages the test will map fixed views over later, held meanwhile so the
 * kernel cannot place another mapping there (the views replace them). */
static void hold(uint8_t *address, size_t pages)
{
    assert(mmap(address, pages * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == address);
}

/* Anonymous sections registered by the server: their own direct memory,
 * zeroed, shown by every view, inside the region, outside it, across its
 * edge or where the kernel puts one; copy-on-write views get a copy; a view
 * never falls back to the shared memory object; the memory goes back once
 * the object and its last view are gone. base is the region (8 pages, taken
 * again here); outside is the free space before it, with 4 more free pages
 * past the region. */
static void test_sections(void)
{
    const unsigned before = used();
    uint64_t counters[10] = { 0 };
    int shm = memfd_create("section", 0), file = memfd_create("file", 0), cookie;
    uint8_t *outside = space_for(16), *base = outside + 4 * PAGE, *placed;
    pid_t child;
    int status;

    /* The region is pages 4..11 of the space; the pages around it are held
     * for the views outside it. */
    hold(outside, 16);
    assert(!munmap(base, 8 * PAGE) && !sceKernelReserveVirtualRange((void **)&base, 8 * PAGE, 0x90, PAGE));
    __wine_ps5_dmem_region(base, 8 * PAGE);
    assert(shm >= 0 && !ftruncate(shm, 4 * PAGE) && file >= 0 && !ftruncate(file, PAGE));
    assert(pwrite(file, "file", 4, 0) == 4);
    /* A console that refuses to map one block twice keeps shared memory. */
    child = fork();
    if (!child) {
        refuse_alias = 1;
        assert(__wine_ps5_section_create(shm, PAGE) == 0 && used() == before);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    /* Too large for direct memory: shared memory as before. */
    assert(__wine_ps5_section_create(shm, (257ull << 20)) == 0 && used() == before);

    cookie = __wine_ps5_section_create(shm, 3 * PAGE + 5);   /* rounded to 4 pages */
    assert(cookie > 0 && used() == before + 4);
    /* A writable view, then a read-only one: the same bytes, zeroed first
     * (the direct memory still held an earlier test's 0x33). */
    assert(__wine_ps5_mmap(base, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm, 0) == base);
    assert(base[0] == 0 && base[2 * PAGE - 1] == 0 && used() == before + 4);
    memset(base, 0x61, 2 * PAGE);
    assert(__wine_ps5_mmap(base + 4 * PAGE, 2 * PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, shm, 0) ==
           base + 4 * PAGE);
    assert(base[4 * PAGE + 10] == 0x61 && used() == before + 4);
    /* A view at an offset into the section. */
    assert(__wine_ps5_mmap(base + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm,
                           3 * PAGE) == base + 2 * PAGE);
    base[2 * PAGE] = 0x62;
    /* Past the section's end there is no direct memory, and never the
     * object: refused, nothing mapped. */
    errno = 0;
    assert(__wine_ps5_mmap(base + 3 * PAGE, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, shm, 4 * PAGE) ==
           MAP_FAILED && errno == ENOMEM);
    /* Another file's shared view is still that file. */
    assert(__wine_ps5_mmap(base + 7 * PAGE, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, file, 0) ==
           base + 7 * PAGE && !memcmp(base + 7 * PAGE, "file", 4));
    /* Copy-on-write: a copy of the section's bytes in fresh direct memory. */
    assert(__wine_ps5_mmap(base + 6 * PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, shm, 0) ==
           base + 6 * PAGE);
    assert(base[6 * PAGE] == 0x61 && used() == before + 5);
    base[6 * PAGE] = 0x70;
    assert(base[0] == 0x61 && base[4 * PAGE] == 0x61);
    assert(__wine_ps5_memory_stats(counters, 10) == 10 && counters[8] == 4 * PAGE && counters[9] >= 4 * PAGE);

    /* Views outside every region map the same direct memory, not the
     * object (which still holds zeros): a fixed one before the region, one
     * straddling the region's end, one the kernel places. Writes through
     * any show through all; no direct memory is taken. */
    assert(__wine_ps5_mmap(outside, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm, 0) == outside);
    assert(SHOWS(outside, 5) == 0x61 && used() == before + 5);
    SHOWS(outside, 5) = 0x63;
    assert(SHOWS(base, 5) == 0x63 && SHOWS(base, 4 * PAGE + 5) == 0x63);
    assert(__wine_ps5_mmap(base + 7 * PAGE, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm, 0) ==
           base + 7 * PAGE);
    assert(SHOWS(base, 7 * PAGE + 5) == 0x63 && SHOWS(base, 8 * PAGE + 100) == 0x61 && used() == before + 5);
    SHOWS(base, 8 * PAGE + 1) = 0x64;
    assert(SHOWS(base, PAGE + 1) == 0x64 && SHOWS(base, 1) == 0x61);
    placed = __wine_ps5_mmap(NULL, PAGE, PROT_READ, MAP_SHARED, shm, 0);
    assert(placed != MAP_FAILED && SHOWS(placed, 5) == 0x63 && used() == before + 5);
    assert(!__wine_ps5_munmap(placed, PAGE));
    /* Private views outside the regions and where the kernel puts them hold
     * a copy in ordinary memory: writes stay there. */
    assert(__wine_ps5_mmap(outside + PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, shm, 0) ==
           outside + PAGE);
    assert(SHOWS(outside, PAGE + 5) == 0x63 && used() == before + 5);
    SHOWS(outside, PAGE + 5) = 0x71;
    assert(SHOWS(base, 5) == 0x63);
    placed = __wine_ps5_mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, shm, 0);
    assert(placed != MAP_FAILED && SHOWS(placed, 5) == 0x63 && used() == before + 5);
    assert(!__wine_ps5_munmap(placed, PAGE) && !__wine_ps5_munmap(outside + PAGE, PAGE));
    /* A fixed mapping of something else over an outside view ends it. */
    assert(__wine_ps5_mmap(outside, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
           outside && SHOWS(outside, 5) == 0);
    assert(!__wine_ps5_munmap(outside, PAGE));

    /* The object goes; its views keep the memory. */
    __wine_ps5_section_release(cookie);
    assert(used() == before + 5 && SHOWS(base, 2 * PAGE) == 0x62);
    assert(!__wine_ps5_munmap(base + 4 * PAGE, 2 * PAGE) && used() == before + 5);
    assert(!__wine_ps5_munmap(base + 7 * PAGE, 2 * PAGE) && used() == before + 5);
    /* ntdll replaces a view with a reservation rather than unmapping it. */
    assert(__wine_ps5_mmap(base, 2 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                           -1, 0) == base);
    assert(used() == before + 5);
    /* The last view: the section's direct memory goes back. */
    assert(!__wine_ps5_munmap(base + 2 * PAGE, PAGE) && used() == before + 1);
    assert(__wine_ps5_memory_stats(counters, 10) == 10 && counters[8] == 0);
    assert(!__wine_ps5_munmap(base, 8 * PAGE) && used() == before);
    assert(!munmap(outside, 16 * PAGE));
    close(shm);
    close(file);
}

/* A section view inside a region, with the region unmapped under it (patch
 * 0897 gives such regions back): the view ends with the region, so no view
 * points into address space the module no longer owns, and the direct
 * memory goes back once the object is gone too. A partial unmap keeps both
 * the region and the rest of the view. */
static void test_view_region_dropped(void)
{
    uint8_t *region = space_for(8);
    const unsigned before = used();
    int shm = memfd_create("section", 0), cookie;

    assert(shm >= 0 && !ftruncate(shm, 2 * PAGE));
    assert(!sceKernelReserveVirtualRange((void **)&region, 8 * PAGE, 0x90, PAGE));
    __wine_ps5_dmem_region(region, 8 * PAGE);
    cookie = __wine_ps5_section_create(shm, 2 * PAGE);
    assert(cookie > 0 && used() == before + 2);
    assert(__wine_ps5_mmap(region + 2 * PAGE, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm, 0) ==
           region + 2 * PAGE);
    SHOWS(region, 2 * PAGE) = 0x42;
    __wine_ps5_section_release(cookie);
    assert(used() == before + 2);                  /* the view holds it */
    /* Half the region, one page of the view: the region stays the module's
     * and the other page still shows the section. */
    assert(!__wine_ps5_munmap(region, 3 * PAGE) && used() == before + 2);
    assert(SHOWS(region, 3 * PAGE) == 0);          /* still mapped, the section's second page */
    assert(__wine_ps5_mmap(region + 6 * PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                           -1, 0) == region + 6 * PAGE && used() == before + 3);   /* direct memory: owned */
    /* The whole region: the view ends, the memory goes back, the region is
     * gone, and a fixed mapping there is the host's. */
    assert(!__wine_ps5_munmap(region, 8 * PAGE) && used() == before);
    assert(free_space(region, 8 * PAGE));
    assert(__wine_ps5_mmap(region, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
           region && used() == before);
    assert(!__wine_ps5_munmap(region, PAGE));
    close(shm);
}

/* The view table (PW_WINE_SECTION_VIEWS slots here) full: one more view is
 * refused with ENOMEM rather than mapped from the object; unmapping frees
 * the slots again. */
static void test_views_full(void)
{
    enum { SLOTS = 24 };
    uint8_t *space = space_for(SLOTS + 2);
    const unsigned before = used();
    int shm = memfd_create("section", 0), cookie, mapped = 0;

    hold(space, SLOTS + 2);
    assert(shm >= 0 && !ftruncate(shm, PAGE));
    cookie = __wine_ps5_section_create(shm, PAGE);
    assert(cookie > 0 && used() == before + 1);
    while (mapped < SLOTS + 2 &&
           __wine_ps5_mmap(space + mapped * PAGE, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, shm, 0) ==
           space + mapped * PAGE)
        mapped++;
    assert(mapped == SLOTS && errno == ENOMEM);
    assert(used() == before + 1);
    assert(!__wine_ps5_munmap(space + 3 * PAGE, PAGE));
    assert(__wine_ps5_mmap(space + SLOTS * PAGE, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, shm, 0) ==
           space + SLOTS * PAGE);
    assert(!__wine_ps5_munmap(space, (SLOTS + 1) * PAGE) && used() == before + 1);
    __wine_ps5_section_release(cookie);
    assert(used() == before);
    assert(!munmap(space, (SLOTS + 2) * PAGE));
    close(shm);
}

static void test_refused(void)
{
    pid_t child = fork();
    int status;

    if (!child) {
        uint8_t *base = space_for(8);
        refuse_map = 1;
        assert(!sceKernelReserveVirtualRange((void **)&base, 8 * PAGE, 0x90, PAGE));
        __wine_ps5_dmem_region(base, 8 * PAGE);
        /* Refused: the calls are the host's own, the reservation stays. */
        assert(__wine_ps5_mmap(base, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                               -1, 0) == base);
        base[5] = 1;
        assert(used() == 0);
        assert(!__wine_ps5_mprotect(base, PAGE, PROT_READ) && !__wine_ps5_munmap(base, PAGE));
        {
            uint64_t counters[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
            assert(__wine_ps5_memory_stats(counters, 8) == 10 && !counters[0] && !counters[2] && !counters[6]);
        }
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
}

int main(void)
{
    uint8_t *base, *outside, *view;
    char path[] = "/tmp/pw_wine_dmem_ps5_XXXXXX";
    int file;

    phys_fd = memfd_create("pw_wine_dmem_ps5", 0);
    assert(phys_fd >= 0 && !ftruncate(phys_fd, (off_t)PHYS_PAGES * PAGE));
    test_refused();

    /* Before any region every call is the host's. */
    outside = space_for(16);
    base = outside + 4 * PAGE;
    assert(__wine_ps5_mmap(outside, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                           -1, 0) == outside);
    outside[0] = 7;
    assert(!__wine_ps5_munmap(outside, PAGE));
    assert(__wine_ps5_mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED);

    /* The region is ntdll's reserved area: pages 4..11 of the space. */
    assert(!sceKernelReserveVirtualRange((void **)&base, 8 * PAGE, 0x90, PAGE));
    cpu_only_refused = 1;  /* the self-check finds the GPU-bits variant */
    __wine_ps5_dmem_region(base, 8 * PAGE);
    assert(used() == 0);   /* the self-check gave its page back */

    /* A fixed anonymous mapping is direct memory, zeroed. */
    assert(__wine_ps5_mmap(base, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                           -1, 0) == base);
    assert(used() == 2 && base[0] == 0 && base[2 * PAGE - 1] == 0);
    {
        /* The title's counters: backed now, peak (the self-check's page was
         * the first), runs, refusals; then the heap's; then the part below
         * 4 GiB, all of it for a region below 4 GiB. */
        uint64_t counters[8] = { 0 };
        const uint64_t low = (uintptr_t)base < ((uintptr_t)1 << 32) ? 2 * PAGE : 0;
        assert(__wine_ps5_memory_stats(counters, 8) == 10);
        assert(counters[0] == 2 * PAGE && counters[1] == 2 * PAGE && counters[2] == 1 && counters[3] == 0);
        assert(counters[6] == low && counters[7] == low);
        assert(__wine_ps5_memory_stats(counters, 1) == 10);
    }
    memset(base, 0x33, 2 * PAGE);
    /* Read-execute, then back: the bytes stay. */
    assert(!__wine_ps5_mprotect(base, 2 * PAGE, PROT_READ | PROT_EXEC) && base[PAGE] == 0x33);
    assert(!__wine_ps5_mprotect(base + 100, 10, PROT_READ | PROT_WRITE));   /* rounded to its page */
    base[1] = 0x44;
    /* A reservation made writable is committed; PROT_NONE commits nothing. */
    assert(__wine_ps5_mmap(base + 2 * PAGE, 2 * PAGE, PROT_NONE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == base + 2 * PAGE);
    assert(used() == 2);
    assert(!__wine_ps5_mprotect(base + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE) && used() == 3);
    base[2 * PAGE] = 9;

    /* A mapping running past the region: the rest is the host's. */
    assert(__wine_ps5_mmap(base + 6 * PAGE, 4 * PAGE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + 6 * PAGE);
    assert(used() == 5);
    base[9 * PAGE] = 1;
    assert(!__wine_ps5_mprotect(base + 6 * PAGE, 4 * PAGE, PROT_READ));
    assert(!__wine_ps5_munmap(base + 6 * PAGE, 4 * PAGE) && used() == 3);
    assert(free_space(base + 6 * PAGE, 4 * PAGE));

    /* A private file view (an image section) is refused with ENODEV, so
     * ntdll reads the file into the direct memory already there. */
    file = mkstemp(path);
    assert(file >= 0 && !ftruncate(file, PAGE));
    assert(pwrite(file, "file", 4, 0) == 4);
    errno = 0;
    assert(__wine_ps5_mmap(base + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, file, 0) == MAP_FAILED &&
           errno == ENODEV);
    assert(used() == 3 && base[1] == 0x44);
    /* A shared one (shared memory) is mapped where asked and takes the
     * place of the direct memory. */
    view = __wine_ps5_mmap(base + PAGE, PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, file, 0);
    assert(view == base + PAGE && !memcmp(view, "file", 4) && used() == 2);
    assert(!__wine_ps5_mprotect(view, PAGE, PROT_READ | PROT_WRITE));
    view[0] = 'F';
    assert(base[1] == 0x44);
    close(file);
    unlink(path);

    /* munmap gives back the memory and the address space. */
    assert(!__wine_ps5_munmap(base, 8 * PAGE) && used() == 0);
    assert(free_space(base, 8 * PAGE));
    /* A whole region unmapped is no longer the module's: a fixed mapping
     * there is the host's. */
    assert(__wine_ps5_mmap(base, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                           -1, 0) == base && used() == 0);
    assert(!__wine_ps5_munmap(base, PAGE));
    /* ntdll reserves it again at a fixed address and hands it over (patch
     * 0897): committing it by protection backs it, zeroed. */
    assert(!sceKernelReserveVirtualRange((void **)&base, 8 * PAGE, 0x90, PAGE));
    __wine_ps5_dmem_region(base, 8 * PAGE);
    assert(!__wine_ps5_mprotect(base, PAGE, PROT_READ | PROT_WRITE) && used() == 1 && base[0] == 0);
    /* Out of direct memory: the call fails with ENOMEM and the range stays
     * reserved. */
    {
        unsigned char saved[PHYS_PAGES];
        memcpy(saved, allocated, sizeof(saved));
        memset(allocated, 1, sizeof(allocated));
        errno = 0;
        assert(__wine_ps5_mmap(base + PAGE, PAGE, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == ENOMEM);
        assert(__wine_ps5_mprotect(base + 2 * PAGE, PAGE, PROT_READ) == -1 && errno == ENOMEM);
        memcpy(allocated, saved, sizeof(saved));
        assert(!free_space(base + PAGE, PAGE));
        /* A map refused after the GPU-bits variant was chosen fails the
         * call; nothing is written to the unmapped page, nothing leaks. */
        refuse_map = 1;
        errno = 0;
        assert(__wine_ps5_mmap(base + PAGE, PAGE, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == ENOMEM);
        assert(!memcmp(allocated, saved, sizeof(saved)));
        refuse_map = 0;
    }
    assert(!__wine_ps5_munmap(base, 8 * PAGE) && used() == 0);
    test_sections();
    test_view_region_dropped();
    test_views_full();
    printf("wine dmem ps5 glue passed: pass-through before and outside the regions, commit, protect, "
           "a mapping across the edge, private and shared file views, munmap, a region given back and taken again, "
           "a refused self-check, and anonymous sections in direct memory (shared, copy-on-write and offset views, "
           "views outside the regions, across an edge and placed by the kernel, never the object, release "
           "with the last view, a region dropped under a view, a full view table, too large, refused aliasing)\n");
    return 0;
}
