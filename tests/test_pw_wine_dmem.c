/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * pw_wine_dmem over a model of the console kernel. Physical direct memory is
 * a memfd, so a mapping keeps its bytes across protection changes exactly
 * as the console's does, and freshly allocated memory is filled with junk
 * (the console does not promise cleared pages). The model enforces the
 * kernel's rules: a reservation refuses to overlap anything, a fixed map
 * must land on a reservation, only a mapped page can be protected, and
 * released memory must be allocated and no longer mapped. A random workload
 * is checked page by page against a reference.
 */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_dmem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { PAGE = 16384, PAGES = 512, PHYS_PAGES = 1024, RUNS = 512, SMALL_TABLE = 64 };
enum { FREE, RESERVED, MAPPED, FOREIGN };

static uint8_t *space;                    /* PAGES host pages the model owns */
static int phys_fd;
static unsigned state[PAGES], prot_of[PAGES];
static int64_t phys_of[PAGES];            /* the direct page a mapped page shows */
static unsigned allocated[PHYS_PAGES];
static unsigned fail_allocate, fail_map, fail_protect;

static unsigned page_of(uintptr_t address) { return (unsigned)((address - (uintptr_t)space) / PAGE); }
static int host_prot(unsigned protection)
{
    return (protection & 1 ? PROT_READ : 0) | (protection & 2 ? PROT_WRITE : 0) |
           (protection & 4 ? PROT_EXEC : 0);
}
static int mapped_anywhere(int64_t phys)
{
    for (unsigned p = 0; p < PAGES; p++)
        if (state[p] == MAPPED && phys_of[p] == phys) return 1;
    return 0;
}

static int model_reserve(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    for (unsigned p = page_of(address); p < page_of(address + bytes); p++)
        if (state[p] != FREE) return -1;
    for (unsigned p = page_of(address); p < page_of(address + bytes); p++) state[p] = RESERVED;
    return 0;
}
static int model_allocate(void *context, size_t bytes, int64_t *offset)
{
    unsigned count = (unsigned)(bytes / PAGE), run = 0;
    (void)context;
    if (fail_allocate && !--fail_allocate) return -1;
    for (unsigned p = 0; p < PHYS_PAGES; p++) {
        run = allocated[p] ? 0 : run + 1;
        if (run == count) {
            unsigned first = p + 1 - count;
            uint8_t junk[PAGE];
            memset(junk, 0xa5, sizeof(junk));
            for (unsigned q = first; q <= p; q++) {
                allocated[q] = 1;
                assert(pwrite(phys_fd, junk, PAGE, (off_t)q * PAGE) == PAGE);
            }
            *offset = (int64_t)first * PAGE;
            return 0;
        }
    }
    return -1;
}
static int model_map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    (void)context;
    if (fail_map && !--fail_map) return -1;
    for (unsigned p = page_of(address); p < page_of(address + bytes); p++)
        if (state[p] != RESERVED || !allocated[(offset + (int64_t)(p - page_of(address)) * PAGE) / PAGE])
            return -1;
    assert(mmap((void *)address, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, phys_fd,
                offset) == (void *)address);
    for (unsigned p = page_of(address), i = 0; p < page_of(address + bytes); p++, i++) {
        state[p] = MAPPED;
        prot_of[p] = 3;
        phys_of[p] = offset + (int64_t)i * PAGE;
    }
    return 0;
}
static int model_protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    (void)context;
    if (fail_protect && !--fail_protect) return -1;
    for (unsigned p = page_of(address); p < page_of(address + bytes); p++)
        if (state[p] != MAPPED && state[p] != FOREIGN) return -1;
    assert(!mprotect((void *)address, bytes, host_prot(protection)));
    for (unsigned p = page_of(address); p < page_of(address + bytes); p++) prot_of[p] = protection;
    return 0;
}
static int model_unmap(void *context, uintptr_t address, size_t bytes)
{
    (void)context;
    assert(mmap((void *)address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
           (void *)address);
    for (unsigned p = page_of(address); p < page_of(address + bytes); p++) state[p] = FREE;
    return 0;
}
static int model_release(void *context, int64_t offset, size_t bytes)
{
    (void)context;
    for (int64_t phys = offset; phys < offset + (int64_t)bytes; phys += PAGE) {
        if (!allocated[phys / PAGE] || mapped_anywhere(phys)) return -1;
        allocated[phys / PAGE] = 0;
    }
    return 0;
}

static const PwWineDmemOps ops = {
    NULL, PAGE, model_reserve, model_allocate, model_map, model_protect, model_unmap, model_release,
};
static PwWineDmem dmem;
static PwWineDmemRun runs[RUNS];

static uintptr_t at(unsigned page) { return (uintptr_t)space + (uintptr_t)page * PAGE; }
/* Where it can, the model's space straddles 4 GiB at this page, inside the
 * first region, so the below-4-GiB counters see runs on both sides and
 * across it. ASan keeps that range for itself; there the space goes
 * anywhere and the counters are checked on one side only. */
#define FOUR_GIB ((uintptr_t)1 << 32)
enum { LOW_PAGES = 100 };
static int straddles;
static unsigned allocated_pages(void)
{
    unsigned count = 0;
    for (unsigned p = 0; p < PHYS_PAGES; p++) count += allocated[p];
    return count;
}
/* The table, the model and the counters describe the same memory. */
static void check_consistent(void)
{
    unsigned mapped = 0, mapped_low = 0;
    PwWineDmemStats stats;

    for (uint32_t i = 0; i < dmem.run_count; i++) {
        const PwWineDmemRun *run = &dmem.runs[i];
        assert(!i || dmem.runs[i - 1].address + dmem.runs[i - 1].bytes <= run->address);
        for (size_t b = 0; b < run->bytes; b += PAGE) {
            unsigned p = page_of(run->address + b);
            if (run->offset == PW_WINE_DMEM_CALLER) assert(state[p] == FOREIGN);
            else assert(state[p] == MAPPED && phys_of[p] == run->offset + (int64_t)b);
        }
    }
    for (unsigned p = 0; p < PAGES; p++) {
        mapped += state[p] == MAPPED;
        mapped_low += state[p] == MAPPED && at(p) < FOUR_GIB;
    }
    pw_wine_dmem_stats(&dmem, &stats);
    assert(stats.backed_bytes == (uint64_t)mapped * PAGE);
    assert(stats.low_backed_bytes == (uint64_t)mapped_low * PAGE);
    assert(stats.peak_low_backed_bytes >= stats.low_backed_bytes &&
           stats.peak_low_backed_bytes <= stats.peak_backed_bytes);
    assert(allocated_pages() == mapped);
    assert(stats.runs == dmem.run_count);
}

static void test_basics(void)
{
    /* Two regions: pages 16..271 and 300..331; outside them nothing works. */
    assert(pw_wine_dmem_add_region(&dmem, at(16), 256 * PAGE, 0) == 0);
    assert(pw_wine_dmem_add_region(&dmem, at(300), 32 * PAGE, 0) == 0);
    assert(pw_wine_dmem_add_region(&dmem, at(310), PAGE, 0) == -1);     /* overlaps */
    assert(pw_wine_dmem_add_region(&dmem, at(400) + 1, PAGE, 1) == -1); /* unaligned */
    /* Pieces: outside, inside, then outside again. */
    size_t piece;
    assert(pw_wine_dmem_split(&dmem, at(10), 20 * PAGE, &piece) == 0 && piece == 6 * PAGE);
    assert(pw_wine_dmem_split(&dmem, at(16), 300 * PAGE, &piece) == 1 && piece == 256 * PAGE);
    assert(pw_wine_dmem_split(&dmem, at(272), 100 * PAGE, &piece) == 0 && piece == 28 * PAGE);
    assert(pw_wine_dmem_split(&dmem, at(305), 3 * PAGE, &piece) == 1 && piece == 3 * PAGE);
    /* A range the caller reserved itself is taken as it is. */
    assert(model_reserve(NULL, at(340), 4 * PAGE) == 0);
    assert(pw_wine_dmem_add_region(&dmem, at(340), 4 * PAGE, 1) == 0);
    assert(pw_wine_dmem_replace(&dmem, at(341), PAGE, 3) == 0 && state[341] == MAPPED);
    assert(pw_wine_dmem_replace(&dmem, at(341), PAGE, 0) == 0 && state[341] == RESERVED);
    /* Adjacent regions join, so a range may span what were two. */
    assert(model_reserve(NULL, at(344), 2 * PAGE) == 0);
    assert(pw_wine_dmem_add_region(&dmem, at(344), 2 * PAGE, 1) == 0 && dmem.regions == 3);
    assert(pw_wine_dmem_owns(&dmem, at(343), 2 * PAGE));
    assert(pw_wine_dmem_add_region(&dmem, at(338), 3 * PAGE, 0) == -1);  /* 340 is reserved */
    assert(model_reserve(NULL, at(338), 2 * PAGE) == 0);
    assert(pw_wine_dmem_add_region(&dmem, at(338), 3 * PAGE, 1) == 0 && dmem.regions == 3);
    assert(pw_wine_dmem_owns(&dmem, at(338), 8 * PAGE) && !pw_wine_dmem_owns(&dmem, at(338), 9 * PAGE));
    assert(state[16] == RESERVED && state[271] == RESERVED && state[272] == FREE);
    assert(pw_wine_dmem_owns(&dmem, at(16), 256 * PAGE) && !pw_wine_dmem_owns(&dmem, at(270), 4 * PAGE));
    assert(pw_wine_dmem_replace(&dmem, at(8), PAGE, 3) == -1);
    assert(pw_wine_dmem_protect(&dmem, at(16), PAGE + 1, 3) == -1);
    assert(pw_wine_dmem_protect(&dmem, at(16), 0, 3) == -1);

    /* Fresh memory is zero despite the junk allocate leaves. */
    assert(pw_wine_dmem_replace(&dmem, at(20), 4 * PAGE, 3) == 0);
    for (size_t b = 0; b < 4 * PAGE; b++) assert(((uint8_t *)at(20))[b] == 0);
    memset((void *)at(20), 0x11, 4 * PAGE);
    /* Protecting keeps the bytes; the page below the run is backed too. */
    assert(pw_wine_dmem_protect(&dmem, at(19), 3 * PAGE, 1) == 0);
    assert(prot_of[19] == 1 && prot_of[21] == 1 && prot_of[22] == 3);
    assert(((uint8_t *)at(19))[0] == 0 && ((uint8_t *)at(21))[PAGE - 1] == 0x11);
    assert(pw_wine_dmem_backed(&dmem, at(18), 8 * PAGE) == 5 * PAGE);
    check_consistent();
    /* Protection 0 on a gap leaves it reserved; on backed pages it keeps them. */
    assert(pw_wine_dmem_protect(&dmem, at(24), 2 * PAGE, 0) == 0 && state[24] == RESERVED);
    assert(pw_wine_dmem_protect(&dmem, at(22), PAGE, 0) == 0 && state[22] == MAPPED && prot_of[22] == 0);
    assert(pw_wine_dmem_protect(&dmem, at(22), PAGE, 3) == 0 && ((uint8_t *)at(22))[7] == 0x11);
    /* Freeing the middle splits the run and releases exactly that page. */
    assert(pw_wine_dmem_replace(&dmem, at(21), PAGE, 0) == 0);
    assert(state[21] == RESERVED && allocated_pages() == 4);
    check_consistent();
    /* Replacing keeps nothing: the page is zero again. */
    assert(pw_wine_dmem_replace(&dmem, at(22), PAGE, 7) == 0 && prot_of[22] == 7);
    assert(((uint8_t *)at(22))[7] == 0);
    check_consistent();
    /* The caller maps a file over backed pages: their direct memory goes
     * back, the file keeps its protection changes and is never backed, and
     * a replace removes it like anything else. */
    const unsigned before = allocated_pages();
    assert(pw_wine_dmem_replace(&dmem, at(40), 4 * PAGE, 3) == 0 && allocated_pages() == before + 4);
    assert(mmap((void *)at(41), 2 * PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
           (void *)at(41));
    state[41] = state[42] = FOREIGN;
    assert(pw_wine_dmem_adopt(&dmem, at(41), 2 * PAGE) == 0 && allocated_pages() == before + 2);
    const uint32_t runs_now = dmem.run_count;
    assert(pw_wine_dmem_adopt(&dmem, at(41), PAGE) == 0 && dmem.run_count == runs_now);
    assert(pw_wine_dmem_adopt(&dmem, at(42), PAGE) == 0 && dmem.run_count == runs_now);
    assert(pw_wine_dmem_backed(&dmem, at(40), 4 * PAGE) == 2 * PAGE);
    assert(pw_wine_dmem_protect(&dmem, at(40), 4 * PAGE, 1) == 0 && prot_of[41] == 1 && prot_of[43] == 1);
    assert(pw_wine_dmem_adopt(&dmem, at(8), PAGE) == -1);
    check_consistent();
    assert(pw_wine_dmem_replace(&dmem, at(40), 4 * PAGE, 3) == 0 && state[41] == MAPPED);
    check_consistent();
    assert(pw_wine_dmem_replace(&dmem, at(16), 256 * PAGE, 0) == 0);
    assert(allocated_pages() == 0 && dmem.run_count == 0);
    check_consistent();
}

static void test_merge_and_failures(void)
{
    PwWineDmemStats stats;

    /* Consecutive commits take consecutive direct memory: one run. */
    for (unsigned p = 0; p < 8; p++) assert(pw_wine_dmem_protect(&dmem, at(100 + p), PAGE, 3) == 0);
    assert(dmem.run_count == 1 && dmem.runs[0].bytes == 8 * PAGE);
    /* A gap filled between two runs joins all three. */
    assert(pw_wine_dmem_replace(&dmem, at(103), PAGE, 0) == 0 && dmem.run_count == 2);
    assert(pw_wine_dmem_protect(&dmem, at(103), PAGE, 3) == 0);
    check_consistent();
    /* Each kernel failure leaves a consistent table, nothing leaked, and
     * is counted once. */
    pw_wine_dmem_stats(&dmem, &stats);
    const uint64_t failures = stats.failures;
    fail_allocate = 1;
    assert(pw_wine_dmem_protect(&dmem, at(120), PAGE, 3) == -1 && state[120] == RESERVED);
    fail_map = 1;
    assert(pw_wine_dmem_replace(&dmem, at(121), PAGE, 3) == -1 && state[121] == RESERVED);
    fail_protect = 1;
    assert(pw_wine_dmem_replace(&dmem, at(122), PAGE, 1) == -1 && state[122] == RESERVED);
    fail_protect = 1;
    assert(pw_wine_dmem_protect(&dmem, at(100), PAGE, 1) == -1);
    check_consistent();
    pw_wine_dmem_stats(&dmem, &stats);
    assert(stats.failures == failures + 4);
    /* A full table refuses work that could need an entry, and still frees. */
    assert(pw_wine_dmem_replace(&dmem, at(16), 256 * PAGE, 0) == 0);
    dmem.run_capacity = SMALL_TABLE;
    for (unsigned i = 0; i < SMALL_TABLE; i++)
        assert(pw_wine_dmem_protect(&dmem, at(16 + 2 * i), PAGE, 3) == 0);
    assert(dmem.run_count == SMALL_TABLE);
    assert(pw_wine_dmem_protect(&dmem, at(17), PAGE, 3) == -1);
    assert(pw_wine_dmem_replace(&dmem, at(17), PAGE, 3) == -1);
    assert(pw_wine_dmem_protect(&dmem, at(16), PAGE, 1) == 0);   /* no new entry needed */
    assert(pw_wine_dmem_replace(&dmem, at(16), 256 * PAGE, 0) == 0);
    dmem.run_capacity = RUNS;
    check_consistent();
}

/* Random replace/protect/adopt/write against a byte-per-page reference. */
static void test_random(void)
{
    enum { NOTHING, BACKED, CALLER };
    static uint8_t expected[PAGES];
    static unsigned holds[PAGES];
    uint64_t seed = 0x9e3779b97f4a7c15ull;

    for (unsigned round = 0; round < 20000; round++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        unsigned first = 16 + (unsigned)(seed % 250), count = 1 + (unsigned)(seed >> 12) % 6;
        unsigned protection = (unsigned)(seed >> 20) % 8, kind = (unsigned)(seed >> 24) % 4;
        if (first + count > 272) count = 272 - first;
        if (kind == 0) {
            assert(pw_wine_dmem_replace(&dmem, at(first), count * PAGE, protection) == 0);
            for (unsigned p = first; p < first + count; p++) {
                expected[p] = 0;
                holds[p] = protection ? BACKED : NOTHING;
            }
        } else if (kind == 1) {
            assert(pw_wine_dmem_protect(&dmem, at(first), count * PAGE, protection) == 0);
            for (unsigned p = first; p < first + count; p++)
                if (holds[p] == NOTHING && protection) { holds[p] = BACKED; expected[p] = 0; }
        } else if (kind == 2) {
            /* A file view: the caller's own fixed mapping, then adopt. */
            assert(mmap((void *)at(first), count * PAGE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void *)at(first));
            memset((void *)at(first), 0x5c, count * PAGE);
            for (unsigned p = first; p < first + count; p++) {
                state[p] = FOREIGN;
                prot_of[p] = 3;
                holds[p] = CALLER;
                expected[p] = 0x5c;
            }
            assert(pw_wine_dmem_adopt(&dmem, at(first), count * PAGE) == 0);
        } else {
            for (unsigned p = first; p < first + count; p++)
                if (holds[p] != NOTHING && (prot_of[p] & 2)) {
                    expected[p] = (uint8_t)(seed >> 32);
                    memset((void *)at(p), expected[p], PAGE);
                }
        }
        for (unsigned p = 16; p < 272; p++) {
            assert((state[p] == MAPPED) == (holds[p] == BACKED));
            assert((state[p] == FOREIGN) == (holds[p] == CALLER));
            if (holds[p] != NOTHING && (prot_of[p] & 1))
                assert(((uint8_t *)at(p))[0] == expected[p] && ((uint8_t *)at(p))[PAGE - 1] == expected[p]);
        }
        if (round % 1000 == 0) check_consistent();
    }
    assert(pw_wine_dmem_replace(&dmem, at(16), 256 * PAGE, 0) == 0);
    check_consistent();
}

int main(void)
{
    PwWineDmemStats stats;

    space = mmap((void *)(FOUR_GIB - (uintptr_t)LOW_PAGES * PAGE), (size_t)(PAGES + 1) * PAGE, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    straddles = space == (uint8_t *)(FOUR_GIB - (uintptr_t)LOW_PAGES * PAGE);
    if (!straddles) {
        if (space != MAP_FAILED) munmap(space, (size_t)(PAGES + 1) * PAGE);
        space = mmap(NULL, (size_t)(PAGES + 1) * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    assert(space != MAP_FAILED && !((uintptr_t)space % 4096));
    /* Host pages are 4 KiB; the model's 16 KiB pages need that alignment. */
    space = (uint8_t *)(((uintptr_t)space + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
    phys_fd = memfd_create("pw_wine_dmem", 0);
    assert(phys_fd >= 0 && !ftruncate(phys_fd, (off_t)PHYS_PAGES * PAGE));
    assert(pw_wine_dmem_init(&dmem, &ops, runs, 1, 1) == -1);
    assert(pw_wine_dmem_init(&dmem, &ops, runs, RUNS, 1) == 0);
    test_basics();
    test_merge_and_failures();
    test_random();
    pw_wine_dmem_stats(&dmem, &stats);
    /* memory was backed on both sides of 4 GiB */
    assert(!straddles ||
           (stats.peak_low_backed_bytes > 0 && stats.peak_low_backed_bytes < stats.peak_backed_bytes));
    printf("wine dmem passed: regions, commit, protect, replace, split and merged runs, "
           "caller mappings, kernel failures, a full table, 20000 random operations and the "
           "below-4-GiB split%s; peak %u runs, %llu KiB, %llu KiB below 4 GiB\n",
           straddles ? "" : " (one side only)",
           stats.peak_runs, (unsigned long long)(stats.peak_backed_bytes >> 10),
           (unsigned long long)(stats.peak_low_backed_bytes >> 10));
    return 0;
}
