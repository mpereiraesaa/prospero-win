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

enum { PAGE = 0x4000, PHYS_PAGES = 256 };
static int phys_fd, refuse_map, cpu_only_refused;
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
    assert(flags == 0x10 && (protection == 3 || protection == 0xf2));
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
            assert(__wine_ps5_memory_stats(counters, 8) == 8 && !counters[0] && !counters[2] && !counters[6]);
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
        assert(__wine_ps5_memory_stats(counters, 8) == 8);
        assert(counters[0] == 2 * PAGE && counters[1] == 2 * PAGE && counters[2] == 1 && counters[3] == 0);
        assert(counters[6] == low && counters[7] == low);
        assert(__wine_ps5_memory_stats(counters, 1) == 8);
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
    printf("wine dmem ps5 glue passed: pass-through before and outside the regions, commit, protect, "
           "a mapping across the edge, private and shared file views, munmap, a region given back and taken again, "
           "and a refused self-check\n");
    return 0;
}
