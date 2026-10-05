/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The per-thread call table behind PW_WOW_TIMING's sys_top and unix_top
 * (wine/wowprospero/call_top.h): counts and cycles per call number, a full
 * table, and the ranking the report prints. */
#include "../wine/wowprospero/call_top.h"
#include <assert.h>
#include <stdio.h>

static void test_empty(void)
{
    PwCallTop t;
    PwCallTopSlot out[4];

    pw_call_top_clear(&t);
    assert(pw_call_top_rank(&t, out, 4) == 0);
    assert(t.dropped == 0);
}

static void test_accumulate_and_rank(void)
{
    PwCallTop t;
    PwCallTopSlot out[8];

    pw_call_top_clear(&t);
    /* Number 0 is a real call (NtAcceptConnectPort in wow64's table). */
    pw_call_top_add(&t, 0, 5);
    pw_call_top_add(&t, 0x43, 100);
    pw_call_top_add(&t, 0x43, 300);
    pw_call_top_add(&t, 0xd5, 1000);
    pw_call_top_add(&t, 0x1234, 50);
    assert(pw_call_top_rank(&t, out, 8) == 4);
    assert(out[0].key == 0xd5 + 1 && out[0].count == 1 && out[0].cycles == 1000);
    assert(out[1].key == 0x43 + 1 && out[1].count == 2 && out[1].cycles == 400);
    assert(out[2].key == 0x1234 + 1 && out[2].count == 1 && out[2].cycles == 50);
    assert(out[3].key == 0 + 1 && out[3].count == 1 && out[3].cycles == 5);

    /* Fewer rows than entries keeps the largest, in order. */
    assert(pw_call_top_rank(&t, out, 2) == 2);
    assert(out[0].key == 0xd6 && out[1].key == 0x44);
    assert(pw_call_top_rank(&t, out, 1) == 1 && out[0].key == 0xd6);

    /* Equal cycles: the lower number first. */
    pw_call_top_clear(&t);
    pw_call_top_add(&t, 9, 7);
    pw_call_top_add(&t, 3, 7);
    pw_call_top_add(&t, 6, 7);
    assert(pw_call_top_rank(&t, out, 3) == 3);
    assert(out[0].key == 4 && out[1].key == 7 && out[2].key == 10);
    assert(pw_call_top_rank(&t, out, 2) == 2);
    assert(out[0].key == 4 && out[1].key == 7);
}

static void test_full_table(void)
{
    PwCallTop t;
    PwCallTopSlot out[PW_CALL_TOP_SLOTS];
    uint64_t total = 0;

    pw_call_top_clear(&t);
    for (uint32_t n = 0; n < PW_CALL_TOP_SLOTS; n++) pw_call_top_add(&t, n * 7919u, n + 1);
    assert(t.dropped == 0);
    /* Every slot taken: a new number is dropped, a known one still counts. */
    pw_call_top_add(&t, 0xabcdef, 1);
    assert(t.dropped == 1);
    pw_call_top_add(&t, 5 * 7919u, 1000);
    assert(t.dropped == 1);
    assert(pw_call_top_rank(&t, out, PW_CALL_TOP_SLOTS) == PW_CALL_TOP_SLOTS);
    assert(out[0].key == 5 * 7919u + 1 && out[0].count == 2 && out[0].cycles == 1006);
    for (unsigned i = 1; i < PW_CALL_TOP_SLOTS; i++) assert(out[i - 1].cycles >= out[i].cycles);
    for (unsigned i = 0; i < PW_CALL_TOP_SLOTS; i++) total += out[i].cycles;
    assert(total == (uint64_t)PW_CALL_TOP_SLOTS * (PW_CALL_TOP_SLOTS + 1) / 2 + 1000);

    /* The one number without a key. */
    pw_call_top_clear(&t);
    pw_call_top_add(&t, 0xffffffffu, 3);
    assert(t.dropped == 1 && pw_call_top_rank(&t, out, 4) == 0);
}

int main(void)
{
    test_empty();
    test_accumulate_and_rank();
    test_full_table();
    printf("call top passed: counts, cycles, ranking, ties, a full table\n");
    return 0;
}
