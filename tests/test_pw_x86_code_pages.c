/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/wowprospero/code_pages.h"
#include <assert.h>
#include <stdio.h>

static PwX86CodePages pages;

static unsigned marked_words(void)
{
    unsigned count = 0;

    for (unsigned i = 0; i < PW_X86_CODE_PAGE_COUNT / 64; i++) count += pages.words[i] != 0;
    return count;
}

int main(void)
{
    assert(sizeof(pages.words) == 128u * 1024u);
    assert(!pw_x86_code_pages_any(&pages, 0, 0x100000000ull));

    /* A block that stays in one page marks exactly that page. */
    pw_x86_code_pages_mark(&pages, 0x7bc01234, 16);
    assert(pages.words[0x7bc01 / 64] == 1ull << (0x7bc01 % 64) && marked_words() == 1);
    assert(pw_x86_code_pages_any(&pages, 0x7bc01000, 0x1000));
    assert(pw_x86_code_pages_any(&pages, 0x7bc01fff, 1));
    assert(pw_x86_code_pages_any(&pages, 0x7bc00000, 0x1001));
    /* The pages either side, and a range ending just before it, are clean. */
    assert(!pw_x86_code_pages_any(&pages, 0x7bc00000, 0x1000));
    assert(!pw_x86_code_pages_any(&pages, 0x7bc02000, 0x10000));
    assert(!pw_x86_code_pages_any(&pages, 0x7bc01000, 0));

    /* A source span crossing a page boundary marks both pages. */
    pw_x86_code_pages_mark(&pages, 0x01000ff0, 0x20);
    assert(pw_x86_code_pages_any(&pages, 0x01000000, 1));
    assert(pw_x86_code_pages_any(&pages, 0x01001000, 1));
    assert(!pw_x86_code_pages_any(&pages, 0x01002000, 1));
    /* Marking again is idempotent. */
    pw_x86_code_pages_mark(&pages, 0x01000ff0, 0x20);
    assert(marked_words() == 2);

    /* A range spanning many words: bits of the first and last word only
     * where the range covers them, whole words in between. */
    pw_x86_code_pages_mark(&pages, 0x20003000, 0x200000 - 0x3000 + 0x5000);
    assert(pages.words[0x20000 / 64] == ~0ull << 3);
    assert(pages.words[0x20040 / 64] == ~0ull);
    assert(pages.words[0x20200 / 64] == 0x1f);
    assert(!pw_x86_code_pages_any(&pages, 0x20000000, 0x3000));
    assert(pw_x86_code_pages_any(&pages, 0x20000000, 0x3001));
    assert(!pw_x86_code_pages_any(&pages, 0x20205000, 0x100000));
    /* A range starting exactly on a word boundary. */
    pw_x86_code_pages_mark(&pages, 0x40000000, 0x40000);
    assert(pages.words[0x40000 / 64] == ~0ull && pages.words[0x40040 / 64] == 0);

    /* The top page of the guest, a range running past 4 GiB (clamped) and
     * one wholly above it (never guest code). */
    pw_x86_code_pages_mark(&pages, 0xfffff000, 0x10000);
    assert(pages.words[PW_X86_CODE_PAGE_COUNT / 64 - 1] == 1ull << 63);
    assert(pw_x86_code_pages_any(&pages, 0xffff0000, 0x20000));
    assert(pw_x86_code_pages_any(&pages, 0, ~0ull));
    unsigned before = marked_words();
    pw_x86_code_pages_mark(&pages, 0x100000000ull, 0x1000);
    pw_x86_code_pages_mark(&pages, ~0ull - 5, 0x1000);
    assert(marked_words() == before);
    assert(!pw_x86_code_pages_any(&pages, 0x100000000ull, 0x100000000ull));

    /* Clearing leaves nothing marked. */
    pw_x86_code_pages_clear(&pages);
    assert(marked_words() == 0 && !pw_x86_code_pages_any(&pages, 0, 0x100000000ull));

    /* NULL is ignored. */
    pw_x86_code_pages_mark(NULL, 0x1000, 1);
    assert(!pw_x86_code_pages_any(NULL, 0x1000, 1));
    pw_x86_code_pages_clear(NULL);
    printf("x86 code pages passed\n");
    return 0;
}
