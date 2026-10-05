/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it. */
#ifndef PW_CODE_PAGES_H
#define PW_CODE_PAGES_H
#include <stdint.h>

/* The 4 KiB pages of the 32-bit guest address space that translated code was
 * read from. A memory notification (protect, free, flush) that touches none
 * of them cannot make a translation stale, so the translation caches are
 * kept instead of discarded. Marks are atomic and may come from any thread;
 * a mark may outlive its translation, never the other way round.
 *
 * words marks the pages translations were read from while read-only, which
 * trust their source; checked marks the pages translations read while
 * writable, which compare their source with a copy whenever they are
 * entered (PwX86SourceViewWritable) and so need no notification at all,
 * only their page to stay readable. */
enum {
    PW_X86_CODE_PAGE_SHIFT = 12,
    PW_X86_CODE_PAGE_COUNT = 1u << (32 - PW_X86_CODE_PAGE_SHIFT),
};

/* The pages with trusting translations (words) that a protection change made
 * writable, with their bytes' digest at that moment: a page that leaves
 * writable unchanged keeps its translations. */
enum { PW_X86_CODE_ARMED = 256 };

typedef struct PwX86CodePages {
    uint64_t words[PW_X86_CODE_PAGE_COUNT / 64];
    uint64_t checked[PW_X86_CODE_PAGE_COUNT / 64];
    uint64_t writable[PW_X86_CODE_PAGE_COUNT / 64];
    struct { uint32_t page_plus1; uint64_t digest; } armed[PW_X86_CODE_ARMED];
} PwX86CodePages;

static const uint64_t pw_code_pages_end = (uint64_t)PW_X86_CODE_PAGE_COUNT << PW_X86_CODE_PAGE_SHIFT;

/* The pages [*first, *last] that [address, address + bytes) overlaps below
 * 4 GiB; 0 when there are none. */
static inline int pw_code_pages_span(uint64_t address, uint64_t bytes, uint32_t *first, uint32_t *last)
{
    uint64_t end;

    if (!bytes || address >= pw_code_pages_end) return 0;
    end = bytes > pw_code_pages_end - address ? pw_code_pages_end : address + bytes;
    *first = (uint32_t)(address >> PW_X86_CODE_PAGE_SHIFT);
    *last = (uint32_t)((end - 1) >> PW_X86_CODE_PAGE_SHIFT);
    return 1;
}

/* The bits of word index that pages [first, last] cover. */
static inline uint64_t pw_code_pages_bits(uint32_t index, uint32_t first, uint32_t last)
{
    uint32_t low = index * 64 > first ? 0 : first % 64;
    uint32_t high = index * 64 + 63 < last ? 63 : last % 64;

    return (~0ull >> (63 - high)) & (~0ull << low);
}

static inline void pw_code_pages_set(uint64_t *bitmap, uint64_t address, uint64_t bytes)
{
    uint32_t first, last;

    if (!pw_code_pages_span(address, bytes, &first, &last)) return;
    for (uint32_t index = first / 64; index <= last / 64; index++) {
        uint64_t bits = pw_code_pages_bits(index, first, last);

        /* Translations mostly revisit marked pages; skip the locked write. */
        if ((__atomic_load_n(&bitmap[index], __ATOMIC_RELAXED) & bits) != bits)
            __atomic_fetch_or(&bitmap[index], bits, __ATOMIC_SEQ_CST);
    }
}

/* Mark every page [address, address + bytes) overlaps, up to 4 GiB, as read
 * by translations that trust their source. */
static inline void pw_x86_code_pages_mark(PwX86CodePages *pages, uint64_t address, uint64_t bytes)
{
    if (pages) pw_code_pages_set(pages->words, address, bytes);
}

/* Mark every page [address, address + bytes) overlaps as read by
 * translations that check their source. */
static inline void pw_x86_code_pages_mark_checked(PwX86CodePages *pages, uint64_t address, uint64_t bytes)
{
    if (pages) pw_code_pages_set(pages->checked, address, bytes);
}

/* 1 when page is marked either way, else 0. */
static inline int pw_x86_code_page_marked(const PwX86CodePages *pages, uint32_t page)
{
    return (int)(((__atomic_load_n(&pages->words[page / 64], __ATOMIC_SEQ_CST) |
                   __atomic_load_n(&pages->checked[page / 64], __ATOMIC_SEQ_CST)) >> (page % 64)) & 1);
}

/* FNV-1a over a page's 64-bit words. */
static inline uint64_t pw_code_page_digest(const uint8_t *bytes)
{
    uint64_t hash = 0xcbf29ce484222325ull;

    for (unsigned i = 0; i < (1u << PW_X86_CODE_PAGE_SHIFT); i += 8) {
        uint64_t word;

        __builtin_memcpy(&word, bytes + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

/* After a protection change on marked page, with its protection now (bytes
 * NULL when it can't be read): 1 when its translations may be stale, so every
 * translation must go, else 0. Translations that check their source never
 * are, as long as they can still read it. For the trusting ones, a page that
 * was read-only since it was translated or last seen can't have changed;
 * making it writable records its digest, and while it stays or leaves
 * writable it is stale only if its bytes differ from that digest.
 * Hooking code reads and patches through exactly that pair of calls (Framerate
 * Vigilante reads a pointer in San Andreas's code that way every frame). The
 * caller holds the lock that serialises notifications. */
static inline int pw_x86_code_page_protect_stale(PwX86CodePages *pages, uint32_t page, int writable,
                                                 const uint8_t *bytes)
{
    const uint64_t bit = 1ull << (page % 64);
    const int was_writable = (__atomic_load_n(&pages->writable[page / 64], __ATOMIC_SEQ_CST) & bit) != 0;
    unsigned slot = (page * 2654435761u) % PW_X86_CODE_ARMED, free_slot = PW_X86_CODE_ARMED;
    unsigned found = PW_X86_CODE_ARMED;

    if (!bytes && (__atomic_load_n(&pages->checked[page / 64], __ATOMIC_SEQ_CST) & bit)) return 1;
    if (!(__atomic_load_n(&pages->words[page / 64], __ATOMIC_SEQ_CST) & bit)) return 0;

    for (unsigned probe = 0; probe < PW_X86_CODE_ARMED; probe++, slot = (slot + 1) % PW_X86_CODE_ARMED) {
        if (pages->armed[slot].page_plus1 == page + 1) { found = slot; break; }
        if (!pages->armed[slot].page_plus1) { if (free_slot == PW_X86_CODE_ARMED) free_slot = slot; break; }
    }
    if (!was_writable && found == PW_X86_CODE_ARMED) {
        if (!writable) return 0;
        if (!bytes || free_slot == PW_X86_CODE_ARMED) return 1;
        pages->armed[free_slot].page_plus1 = page + 1;
        pages->armed[free_slot].digest = pw_code_page_digest(bytes);
        __atomic_fetch_or(&pages->writable[page / 64], bit, __ATOMIC_SEQ_CST);
        return 0;
    }
    if (found == PW_X86_CODE_ARMED || !bytes || pw_code_page_digest(bytes) != pages->armed[found].digest)
        return 1;
    if (!writable) {
        /* Back to read-only: drop the entry, keeping the probe chain whole. */
        unsigned hole = found, next = (found + 1) % PW_X86_CODE_ARMED;

        pages->armed[hole].page_plus1 = 0;
        for (unsigned step = 1; step < PW_X86_CODE_ARMED && pages->armed[next].page_plus1; step++) {
            unsigned home = ((pages->armed[next].page_plus1 - 1) * 2654435761u) % PW_X86_CODE_ARMED;

            if ((next > hole && (home <= hole || home > next)) || (next < hole && home <= hole && home > next)) {
                pages->armed[hole] = pages->armed[next];
                pages->armed[next].page_plus1 = 0;
                hole = next;
            }
            next = (next + 1) % PW_X86_CODE_ARMED;
        }
        __atomic_fetch_and(&pages->writable[page / 64], ~bit, __ATOMIC_SEQ_CST);
    }
    return 0;
}

/* 1 when any page [address, address + bytes) overlaps is marked, else 0;
 * the part at or above 4 GiB never holds guest code. */
static inline int pw_x86_code_pages_any(const PwX86CodePages *pages, uint64_t address, uint64_t bytes)
{
    uint32_t first, last;

    if (!pages || !pw_code_pages_span(address, bytes, &first, &last)) return 0;
    for (uint32_t index = first / 64; index <= last / 64; index++)
        if ((__atomic_load_n(&pages->words[index], __ATOMIC_SEQ_CST) |
             __atomic_load_n(&pages->checked[index], __ATOMIC_SEQ_CST)) & pw_code_pages_bits(index, first, last))
            return 1;
    return 0;
}

/* Unmark every page and forget the digests, for when every translation is
 * discarded. */
static inline void pw_x86_code_pages_clear(PwX86CodePages *pages)
{
    if (!pages) return;
    for (uint32_t index = 0; index < PW_X86_CODE_PAGE_COUNT / 64; index++) {
        __atomic_store_n(&pages->words[index], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&pages->checked[index], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&pages->writable[index], 0, __ATOMIC_RELAXED);
    }
    for (unsigned slot = 0; slot < PW_X86_CODE_ARMED; slot++) pages->armed[slot].page_plus1 = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#endif
