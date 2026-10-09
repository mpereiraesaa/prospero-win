/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_dmem.h"
#include <string.h>

enum { READ_WRITE = PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE };

static void lock(PwWineDmem *d)
{
    while (__atomic_exchange_n(&d->lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&d->lock, __ATOMIC_RELAXED))
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#else
            ;
#endif
}
static void unlock(PwWineDmem *d) { __atomic_store_n(&d->lock, 0, __ATOMIC_RELEASE); }

static uintptr_t end_of(const PwWineDmemRun *run) { return run->address + run->bytes; }

/* The first run that ends above address. */
static uint32_t first_after(const PwWineDmem *d, uintptr_t address)
{
    uint32_t low = 0, high = d->run_count;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if (end_of(&d->runs[mid]) <= address) low = mid + 1; else high = mid;
    }
    return low;
}

static int valid(const PwWineDmem *d, uintptr_t address, size_t bytes)
{
    const size_t page = d->ops.page;
    if (!bytes || address % page || bytes % page || address + bytes < address) return 0;
    for (unsigned i = 0; i < d->regions; i++)
        if (address >= d->region_low[i] && address + bytes <= d->region_high[i]) return 1;
    return 0;
}

/* bytes at address became backed (sign 1) or stopped being (-1). */
static void note_backed(PwWineDmem *d, uintptr_t address, size_t bytes, int sign)
{
    const uint64_t four_gib = (uint64_t)1 << 32;
    uint64_t end = (uint64_t)address + bytes;
    uint64_t low = address >= four_gib ? 0 : (end < four_gib ? end : four_gib) - address;

    d->stats.backed_bytes += sign > 0 ? bytes : -(uint64_t)bytes;
    d->stats.low_backed_bytes += sign > 0 ? low : -low;
    if (d->stats.backed_bytes > d->stats.peak_backed_bytes)
        d->stats.peak_backed_bytes = d->stats.backed_bytes;
    if (d->stats.low_backed_bytes > d->stats.peak_low_backed_bytes)
        d->stats.peak_low_backed_bytes = d->stats.low_backed_bytes;
}

static void insert_at(PwWineDmem *d, uint32_t index, PwWineDmemRun run)
{
    memmove(&d->runs[index + 1], &d->runs[index], (d->run_count - index) * sizeof(run));
    d->runs[index] = run;
    if (++d->run_count > d->stats.peak_runs) d->stats.peak_runs = d->run_count;
}

static void remove_at(PwWineDmem *d, uint32_t index)
{
    memmove(&d->runs[index], &d->runs[index + 1], (d->run_count - index - 1) * sizeof(d->runs[0]));
    d->run_count--;
}

/* Drop the runs' share of [low, high), whose mappings are already gone, and
 * release its direct memory. A run cut in the middle becomes two, so the
 * caller keeps one entry free. */
static void forget(PwWineDmem *d, uintptr_t low, uintptr_t high)
{
    uint32_t i = first_after(d, low);

    while (i < d->run_count && d->runs[i].address < high) {
        PwWineDmemRun run = d->runs[i];
        uintptr_t from = run.address > low ? run.address : low;
        uintptr_t to = end_of(&run) < high ? end_of(&run) : high;
        int keep_left = from > run.address, keep_right = to < end_of(&run);

        if (run.offset != PW_WINE_DMEM_CALLER) {
            if (d->ops.release(d->ops.context, run.offset + (int64_t)(from - run.address), to - from))
                d->stats.failures++;
            else
                d->stats.releases++;
            note_backed(d, from, to - from, -1);
        }
        if (keep_left) d->runs[i].bytes = from - run.address;
        if (keep_right) {
            PwWineDmemRun right = { to, end_of(&run) - to, run.offset };
            if (run.offset != PW_WINE_DMEM_CALLER) right.offset += (int64_t)(to - run.address);
            if (keep_left) insert_at(d, ++i, right); else d->runs[i] = right;
        }
        if (keep_left || keep_right) i++; else remove_at(d, i);
    }
}

/* Record a run whose range no run touches, joining the runs on either side
 * that continue it; one table entry is free. */
static void add_run(PwWineDmem *d, PwWineDmemRun run)
{
    const int caller = run.offset == PW_WINE_DMEM_CALLER;
    uint32_t i = first_after(d, run.address);
    PwWineDmemRun *before = i ? &d->runs[i - 1] : NULL;
    PwWineDmemRun *after = i < d->run_count ? &d->runs[i] : NULL;

    if (before && end_of(before) != run.address) before = NULL;
    if (before && (caller ? before->offset != PW_WINE_DMEM_CALLER
                          : before->offset == PW_WINE_DMEM_CALLER ||
                            before->offset + (int64_t)before->bytes != run.offset))
        before = NULL;
    if (after && after->address != end_of(&run)) after = NULL;
    if (after && (caller ? after->offset != PW_WINE_DMEM_CALLER
                         : after->offset != run.offset + (int64_t)run.bytes))
        after = NULL;
    if (before) {
        before->bytes += run.bytes;
        if (after) {
            before->bytes += after->bytes;
            remove_at(d, i);
        }
    } else if (after) {
        after->address = run.address;
        after->bytes += run.bytes;
        after->offset = run.offset;
    } else {
        insert_at(d, i, run);
    }
}

/* Back a reserved range that no run touches, with one table entry free. */
static int back(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    const PwWineDmemOps *ops = &d->ops;
    int64_t offset;

    if (ops->allocate(ops->context, bytes, &offset)) goto failed;
    d->stats.allocations++;
    if (ops->map(ops->context, address, bytes, offset)) goto release;
    d->stats.maps++;
    if (d->zero_fill) memset((void *)address, 0, bytes);
    if (protection != READ_WRITE) {
        if (ops->protect(ops->context, address, bytes, protection)) {
            (void)ops->unmap(ops->context, address, bytes);
            (void)ops->reserve(ops->context, address, bytes);
            goto release;
        }
        d->stats.protects++;
    }
    note_backed(d, address, bytes, 1);
    /* Direct memory handed out in order often continues the run before. */
    add_run(d, (PwWineDmemRun){ address, bytes, offset });
    return 0;
release:
    (void)ops->release(ops->context, offset, bytes);
failed:
    d->stats.failures++;
    return -1;
}

int pw_wine_dmem_init(PwWineDmem *d, const PwWineDmemOps *ops, PwWineDmemRun *runs,
                      uint32_t capacity, unsigned zero_fill)
{
    if (!d || !ops || !runs || capacity < 2 || !ops->page || (ops->page & (ops->page - 1)) ||
        !ops->reserve || !ops->allocate || !ops->map || !ops->protect || !ops->unmap || !ops->release)
        return -1;
    memset(d, 0, sizeof(*d));
    d->ops = *ops;
    d->runs = runs;
    d->run_capacity = capacity;
    d->zero_fill = !!zero_fill;
    return 0;
}

int pw_wine_dmem_add_region(PwWineDmem *d, uintptr_t address, size_t bytes, int reserved)
{
    int status = -1;

    lock(d);
    if (d->regions < PW_WINE_DMEM_MAX_REGIONS && bytes && !(address % d->ops.page) &&
        !(bytes % d->ops.page) && address + bytes > address &&
        (reserved || !d->ops.reserve(d->ops.context, address, bytes))) {
        uintptr_t low = address, high = address + bytes;
        unsigned kept = 0;

        /* Regions that touch or overlap the new one merge into it. */
        for (unsigned i = 0; i < d->regions; i++) {
            if (d->region_high[i] < low || d->region_low[i] > high) {
                d->region_low[kept] = d->region_low[i];
                d->region_high[kept++] = d->region_high[i];
                continue;
            }
            if (d->region_low[i] < low) low = d->region_low[i];
            if (d->region_high[i] > high) high = d->region_high[i];
        }
        d->region_low[kept] = low;
        d->region_high[kept] = high;
        d->regions = kept + 1;
        status = 0;
    }
    unlock(d);
    return status;
}

int pw_wine_dmem_remove_regions(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    uintptr_t end = address + bytes < address ? UINTPTR_MAX : address + bytes;
    unsigned kept = 0, dropped = 0;

    lock(d);
    for (unsigned i = 0; i < d->regions; i++) {
        uint32_t run = first_after(d, d->region_low[i]);

        if (d->region_low[i] >= address && d->region_high[i] <= end &&
            (run == d->run_count || d->runs[run].address >= d->region_high[i])) {
            dropped++;
            continue;
        }
        d->region_low[kept] = d->region_low[i];
        d->region_high[kept++] = d->region_high[i];
    }
    d->regions = kept;
    unlock(d);
    return (int)dropped;
}

int pw_wine_dmem_split(PwWineDmem *d, uintptr_t address, size_t bytes, size_t *piece)
{
    uintptr_t end = address + bytes < address ? UINTPTR_MAX : address + bytes;
    int owned = 0;

    lock(d);
    for (unsigned i = 0; i < d->regions; i++) {
        if (address >= d->region_low[i] && address < d->region_high[i]) {
            owned = 1;
            if (d->region_high[i] < end) end = d->region_high[i];
        } else if (d->region_low[i] > address && d->region_low[i] < end) {
            end = d->region_low[i];
        }
    }
    unlock(d);
    *piece = end - address;
    return owned;
}

int pw_wine_dmem_owns(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    int owns;

    lock(d);
    owns = valid(d, address, bytes);
    unlock(d);
    return owns;
}

/* Table entries a replace may add: one when it cuts a run in two, one for
 * the new run. */
static uint32_t replace_needs(const PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    uint32_t i = first_after(d, address);
    int split = i < d->run_count && d->runs[i].address < address && end_of(&d->runs[i]) > address + bytes;
    return (uint32_t)split + (protection != 0);
}

int pw_wine_dmem_replace(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    int status = -1;

    protection &= 7;
    lock(d);
    if (!valid(d, address, bytes) ||
        d->run_count + replace_needs(d, address, bytes, protection) > d->run_capacity ||
        d->ops.unmap(d->ops.context, address, bytes)) {
        d->stats.failures++;
        unlock(d);
        return -1;
    }
    forget(d, address, address + bytes);
    /* If the reservation is refused the range is merely free, and a fixed
     * map still takes it. */
    if (d->ops.reserve(d->ops.context, address, bytes)) d->stats.failures++;
    status = protection ? back(d, address, bytes, protection) : 0;
    unlock(d);
    return status;
}

int pw_wine_dmem_map_shared(PwWineDmem *d, uintptr_t address, size_t bytes, int64_t offset,
                            unsigned protection)
{
    protection &= 7;
    lock(d);
    if (!valid(d, address, bytes) ||
        d->run_count + replace_needs(d, address, bytes, 1) > d->run_capacity ||
        d->ops.unmap(d->ops.context, address, bytes)) {
        d->stats.failures++;
        unlock(d);
        return -1;
    }
    forget(d, address, address + bytes);
    /* A fixed map lands on a reservation; refused, the range is merely
     * free and the map still takes it. */
    if (d->ops.reserve(d->ops.context, address, bytes)) d->stats.failures++;
    if (d->ops.map(d->ops.context, address, bytes, offset)) goto failed;
    d->stats.maps++;
    if (protection != READ_WRITE) {
        if (d->ops.protect(d->ops.context, address, bytes, protection)) {
            (void)d->ops.unmap(d->ops.context, address, bytes);
            goto unmapped;
        }
        d->stats.protects++;
    }
    /* The direct memory belongs to whoever shares it out: never released
     * here, like a mapping of the caller's. */
    add_run(d, (PwWineDmemRun){ address, bytes, PW_WINE_DMEM_CALLER });
    unlock(d);
    return 0;
unmapped:
    if (d->ops.reserve(d->ops.context, address, bytes)) d->stats.failures++;
failed:
    d->stats.failures++;
    unlock(d);
    return -1;
}

int pw_wine_dmem_adopt(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    lock(d);
    if (!valid(d, address, bytes) ||
        d->run_count + replace_needs(d, address, bytes, 1) > d->run_capacity) {
        d->stats.failures++;
        unlock(d);
        return -1;
    }
    forget(d, address, address + bytes);
    add_run(d, (PwWineDmemRun){ address, bytes, PW_WINE_DMEM_CALLER });
    unlock(d);
    return 0;
}

int pw_wine_dmem_protect(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    const uintptr_t high = address + bytes;
    uintptr_t at = address;
    uint32_t i;

    protection &= 7;
    lock(d);
    if (!valid(d, address, bytes)) goto failed;
    i = first_after(d, address);
    while (at < high) {
        if (i < d->run_count && d->runs[i].address <= at) {
            uintptr_t to = end_of(&d->runs[i]) < high ? end_of(&d->runs[i]) : high;
            if (d->ops.protect(d->ops.context, at, to - at, protection)) goto failed;
            d->stats.protects++;
            at = to;
            i++;
        } else {
            uintptr_t to = i < d->run_count && d->runs[i].address < high ? d->runs[i].address : high;
            if (protection) {
                if (d->run_count + 1 > d->run_capacity) goto failed;
                if (back(d, at, to - at, protection)) goto counted;
                i = first_after(d, to);
            }
            at = to;
        }
    }
    unlock(d);
    return 0;
failed:
    d->stats.failures++;
counted:
    unlock(d);
    return -1;
}

size_t pw_wine_dmem_backed(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    const uintptr_t high = address + bytes;
    size_t total = 0;

    lock(d);
    for (uint32_t i = first_after(d, address); i < d->run_count && d->runs[i].address < high; i++) {
        if (d->runs[i].offset == PW_WINE_DMEM_CALLER) continue;
        uintptr_t from = d->runs[i].address > address ? d->runs[i].address : address;
        uintptr_t to = end_of(&d->runs[i]) < high ? end_of(&d->runs[i]) : high;
        total += to - from;
    }
    unlock(d);
    return total;
}

void pw_wine_dmem_stats(PwWineDmem *d, PwWineDmemStats *stats)
{
    lock(d);
    *stats = d->stats;
    stats->runs = d->run_count;
    unlock(d);
}
