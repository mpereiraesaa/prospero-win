/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it.
 *
 * PW_WOW_TIMING splits a thread's time between translated code, Unix calls
 * and system calls, but not between the calls themselves. A thread at 22%
 * "sys" may be waiting for another thread, yielding in a spin loop, or
 * sleeping in a frame limiter, and only the call numbers tell these apart.
 * This table keeps, per thread, the count and TSC cycles of each call
 * number seen since the last report, so the report can name the calls that
 * took the most time. It is a small open-addressed table owned by one
 * thread: no locks, no allocation, constant work per call. A number that
 * finds the table full is counted in `dropped` instead. */
#ifndef PW_WOW_CALL_TOP_H
#define PW_WOW_CALL_TOP_H
#include <stdint.h>
#include <string.h>

enum { PW_CALL_TOP_SLOTS = 256 };

typedef struct
{
    uint32_t key;     /* the call number + 1; 0 marks a free slot */
    uint32_t count;
    uint64_t cycles;
} PwCallTopSlot;

typedef struct
{
    PwCallTopSlot slots[PW_CALL_TOP_SLOTS];
    uint32_t dropped;
} PwCallTop;

static inline void pw_call_top_clear(PwCallTop *t)
{
    memset(t, 0, sizeof(*t));
}

static inline void pw_call_top_add(PwCallTop *t, uint32_t number, uint64_t cycles)
{
    uint32_t key = number + 1, i = (number * 2654435761u) >> 24;  /* 8 bits: 256 slots */

    if (!key) { t->dropped++; return; }  /* 0xffffffff has no key */
    for (unsigned probe = 0; probe < PW_CALL_TOP_SLOTS; probe++, i = (i + 1) % PW_CALL_TOP_SLOTS)
    {
        PwCallTopSlot *slot = &t->slots[i];

        if (slot->key == key || !slot->key)
        {
            slot->key = key;
            slot->count++;
            slot->cycles += cycles;
            return;
        }
    }
    t->dropped++;
}

/* The up to `max` slots with the most cycles, most first (ties by number),
 * copied into `out`; returns how many. */
static inline unsigned pw_call_top_rank(const PwCallTop *t, PwCallTopSlot *out, unsigned max)
{
    unsigned n = 0;

    for (unsigned i = 0; i < PW_CALL_TOP_SLOTS; i++)
    {
        const PwCallTopSlot *slot = &t->slots[i];
        unsigned at;

        if (!slot->key) continue;
        for (at = n; at > 0; at--)
        {
            const PwCallTopSlot *prev = &out[at - 1];

            if (prev->cycles > slot->cycles || (prev->cycles == slot->cycles && prev->key < slot->key)) break;
            if (at < max) out[at] = *prev;
        }
        if (at < max)
        {
            out[at] = *slot;
            if (n < max) n++;
        }
    }
    return n;
}

#endif
