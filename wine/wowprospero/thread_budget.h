/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it. */
#ifndef PW_THREAD_BUDGET_H
#define PW_THREAD_BUDGET_H
#include <stddef.h>
#include <stdint.h>

/* What one guest thread's DBT reserves: its translation cache entries, the
 * arena translated code is written to, and the single-instruction fallback's
 * code buffer.
 *
 * Every i386 thread has its own DBT, in memory Wine allocates for it
 * (host_memory.h), which on the console is direct memory: committed as soon
 * as it is allocated, out of up to 12 GiB. The first thread, which runs a
 * game's startup and main loop, gets a 128 MiB arena and 131072 entries
 * (46 MiB): a game's hot code must fit in three quarters of them, or the
 * cache resets, recompiles and stutters every minute or so (Half-Life 2
 * runs about 50000-60000 blocks);
 * later threads, which run far less code (an audio mixer, a loader), get a
 * 32 MiB arena and 16384 entries, so tens of threads fit. A thread whose allocation is
 * refused tries again with everything halved, down to a floor. A full arena
 * is not fatal: the engine is reset and translation continues. */
typedef struct PwWowThreadBudget {
    uint32_t entries;
    size_t arena_bytes;
    size_t hostexec_bytes;
} PwWowThreadBudget;

enum {
    PW_WOW_FIRST_ENTRIES = 131072,
    PW_WOW_OTHER_ENTRIES = 16384,
    PW_WOW_MIN_ENTRIES = 1024,
};
#define PW_WOW_FIRST_ARENA ((size_t)128 << 20)
#define PW_WOW_OTHER_ARENA ((size_t)32 << 20)
#define PW_WOW_MIN_ARENA ((size_t)2 << 20)
#define PW_WOW_FIRST_HOSTEXEC ((size_t)4 << 20)
#define PW_WOW_OTHER_HOSTEXEC ((size_t)1 << 20)
#define PW_WOW_MIN_HOSTEXEC ((size_t)256 << 10)

/* The sizes for a thread's attempt-th try (0 first); 0, or -1 once halving
 * would go below the floor. */
static inline int pw_wow_thread_budget(int first_thread, unsigned attempt, PwWowThreadBudget *out)
{
    PwWowThreadBudget b = {
        first_thread ? PW_WOW_FIRST_ENTRIES : PW_WOW_OTHER_ENTRIES,
        first_thread ? PW_WOW_FIRST_ARENA : PW_WOW_OTHER_ARENA,
        first_thread ? PW_WOW_FIRST_HOSTEXEC : PW_WOW_OTHER_HOSTEXEC,
    };

    for (; attempt; attempt--)
    {
        b.entries /= 2;
        b.arena_bytes /= 2;
        b.hostexec_bytes /= 2;
        if (b.entries < PW_WOW_MIN_ENTRIES || b.arena_bytes < PW_WOW_MIN_ARENA) return -1;
    }
    if (b.hostexec_bytes < PW_WOW_MIN_HOSTEXEC) b.hostexec_bytes = PW_WOW_MIN_HOSTEXEC;
    *out = b;
    return 0;
}

/* Calls setup(context, budget) with each budget in turn until it returns 0.
 * The attempt that worked, or -1 when every budget was refused; *used is
 * the budget that worked. */
static inline int pw_wow_thread_fit(int first_thread, int (*setup)(void *, const PwWowThreadBudget *),
                                    void *context, PwWowThreadBudget *used)
{
    PwWowThreadBudget budget;

    for (unsigned attempt = 0; !pw_wow_thread_budget(first_thread, attempt, &budget); attempt++)
        if (!setup(context, &budget))
        {
            *used = budget;
            return (int)attempt;
        }
    return -1;
}
#endif
