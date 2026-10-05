/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Header-only: the Unix side of the backend and its unit test include it.
 *
 * The hotspot profiler checks its report period at every return from run().
 * On the PS5, clock_gettime is a real system call (about 1 us), and a thread
 * such as DXVK's command stream returns from run() about 18,000 times a
 * frame, so a clock read there made the profiler the bottleneck. The period
 * is kept in TSC ticks instead (the TSC is invariant and shared by the
 * cores on the hosts we run on), and ticks become milliseconds only when a
 * report is written. The rate comes from one calibration against
 * CLOCK_MONOTONIC when the profiler starts. */
#ifndef PW_TSC_CLOCK_H
#define PW_TSC_CLOCK_H
#include <stdint.h>

/* The ticks per second between two (TSC, monotonic ns) points, or 0 when
 * either clock did not move forward. */
static inline uint64_t pw_tsc_rate(uint64_t tsc0, uint64_t ns0, uint64_t tsc1, uint64_t ns1)
{
    if (tsc1 <= tsc0 || ns1 <= ns0) return 0;
    return (uint64_t)((double)(tsc1 - tsc0) * 1e9 / (double)(ns1 - ns0) + 0.5);
}

/* Whole milliseconds in a tick count, truncated like the monotonic clock's
 * tv_nsec / 1000000 was. Exact for any rate below 2^54 ticks per second. */
static inline uint64_t pw_tsc_ms(uint64_t ticks, uint64_t rate)
{
    if (!rate) return 0;
    return ticks / rate * 1000u + ticks % rate * 1000u / rate;
}

/* The fewest ticks that pw_tsc_ms counts as ms milliseconds: a period check
 * against it is one subtraction and one compare. */
static inline uint64_t pw_tsc_ticks(uint64_t ms, uint64_t rate)
{
    return rate / 1000u * ms + (rate % 1000u * ms + 999u) / 1000u;
}

#endif
