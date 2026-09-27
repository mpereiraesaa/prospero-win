/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it. */
#ifndef PW_CODE_PAGES_H
#define PW_CODE_PAGES_H
#include <stdint.h>

/* The 4 KiB pages of the 32-bit guest address space that translated code was
 * read from. A memory notification (protect, free, flush) that touches none
 * of them cannot make a translation stale, so the translation caches are
 * kept instead of discarded. Marks are atomic and may come from any thread;
 * a mark may outlive its translation, never the other way round. */
enum {
    PW_X86_CODE_PAGE_SHIFT = 12,
    PW_X86_CODE_PAGE_COUNT = 1u << (32 - PW_X86_CODE_PAGE_SHIFT),
};

typedef struct PwX86CodePages {
    uint64_t words[PW_X86_CODE_PAGE_COUNT / 64];
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

/* Mark every page [address, address + bytes) overlaps, up to 4 GiB. */
static inline void pw_x86_code_pages_mark(PwX86CodePages *pages, uint64_t address, uint64_t bytes)
{
    uint32_t first, last;

    if (!pages || !pw_code_pages_span(address, bytes, &first, &last)) return;
    for (uint32_t index = first / 64; index <= last / 64; index++) {
        uint64_t bits = pw_code_pages_bits(index, first, last);

        /* Translations mostly revisit marked pages; skip the locked write. */
        if ((__atomic_load_n(&pages->words[index], __ATOMIC_RELAXED) & bits) != bits)
            __atomic_fetch_or(&pages->words[index], bits, __ATOMIC_SEQ_CST);
    }
}

/* 1 when any page [address, address + bytes) overlaps is marked, else 0;
 * the part at or above 4 GiB never holds guest code. */
static inline int pw_x86_code_pages_any(const PwX86CodePages *pages, uint64_t address, uint64_t bytes)
{
    uint32_t first, last;

    if (!pages || !pw_code_pages_span(address, bytes, &first, &last)) return 0;
    for (uint32_t index = first / 64; index <= last / 64; index++)
        if (__atomic_load_n(&pages->words[index], __ATOMIC_SEQ_CST) & pw_code_pages_bits(index, first, last))
            return 1;
    return 0;
}

/* Unmark every page, for when every translation is discarded. */
static inline void pw_x86_code_pages_clear(PwX86CodePages *pages)
{
    if (!pages) return;
    for (uint32_t index = 0; index < PW_X86_CODE_PAGE_COUNT / 64; index++)
        __atomic_store_n(&pages->words[index], 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#endif
