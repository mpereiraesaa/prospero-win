/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_QPC_CLOCK_H
#define PW_QPC_CLOCK_H
#include <stdint.h>
#include <stddef.h>

/* Pointer-free wire layout, identical for PE32 and the Unix provider. */
struct pw_qpc_anchor
{
    uint32_t version;
    uint32_t validated;
    uint64_t tsc;
    uint64_t counter;
    uint64_t frequency;
    uint64_t multiplier;
    uint64_t max_delta;
};
#define PW_QPC_PROCESS_INFO 0x50575101u
#define PW_QPC_VERSION 1u
#define PW_QPC_FREQUENCY UINT64_C(10000000)
#define PW_QPC_MIN_TSC_HZ UINT64_C(20000000)
#define PW_QPC_MAX_TSC_HZ UINT64_C(10000000000)

/* Host-only construction after platform validation. Every anchor expires in
 * at most one second: a stale or migrated clock never extrapolates forever. */
static inline int pw_qpc_make_anchor(struct pw_qpc_anchor *a, uint64_t tsc,
                                    uint64_t counter, uint64_t hz, int validated)
{
    if (!validated || hz < PW_QPC_MIN_TSC_HZ || hz > PW_QPC_MAX_TSC_HZ ||
        counter > INT64_MAX) return 0;
    a->version = PW_QPC_VERSION;
    a->validated = 1;
    a->tsc = tsc;
    a->counter = counter;
    a->frequency = PW_QPC_FREQUENCY;
    a->multiplier = (PW_QPC_FREQUENCY << 32) / hz;
    a->max_delta = hz;
    return 1;
}

/* Readers must receive a coherent snapshot: these fields are not a seqlock.
 * This arithmetic uses only 64-bit multiply and shift on PE32; no 128-bit
 * helper or division on the hot path. A rejected value uses the native API. */
static inline int pw_qpc_read_anchor(const struct pw_qpc_anchor *a, uint64_t tsc,
                                    uint64_t *counter)
{
    uint64_t delta, ticks, low, high, product;
    if (a->version != PW_QPC_VERSION || a->validated != 1 ||
        a->frequency != PW_QPC_FREQUENCY || tsc < a->tsc ||
        !a->multiplier || a->multiplier > UINT32_MAX || a->max_delta < PW_QPC_MIN_TSC_HZ ||
        a->max_delta > PW_QPC_MAX_TSC_HZ || a->counter > INT64_MAX) return 0;
    delta = tsc - a->tsc;
    if (delta > a->max_delta) return 0;
    low = (uint64_t)(uint32_t)delta * (uint32_t)a->multiplier;
    high = (delta >> 32) * a->multiplier;
    if (high > UINT32_MAX) return 0;
    product = high << 32;
    if (low > UINT64_MAX - product) return 0;
    ticks = (low + product) >> 32;
    if (ticks > INT64_MAX - a->counter) return 0;
    *counter = a->counter + ticks;
    return 1;
}

_Static_assert(sizeof(struct pw_qpc_anchor) == 48, "QPC wire size");
_Static_assert(offsetof(struct pw_qpc_anchor, tsc) == 8, "QPC wire offset");
_Static_assert(offsetof(struct pw_qpc_anchor, max_delta) == 40, "QPC wire offset");
#endif
