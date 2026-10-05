/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The hotspot profiler's TSC clock (wine/wowprospero/tsc_clock.h): its
 * report values must match what the monotonic-clock version wrote, for a
 * known tick rate. */
#include "../wine/wowprospero/tsc_clock.h"
#include <assert.h>
#include <stdio.h>

/* Synthetic rates: the PS5's nominal 1.6 GHz, a 3.6 GHz desktop, one with
 * no whole kHz, and the smallest the helpers accept. */
static const uint64_t rates[] = { 1600000000u, 3600000000u, 2999999999u, 1000u, 1u };

static void test_rate(void)
{
    /* 32,000,000 ticks over 20 ms is 1.6 GHz. */
    assert(pw_tsc_rate(1000, 5000000, 32001000, 25000000) == 1600000000u);
    /* An odd span rounds to the nearest tick per second. */
    assert(pw_tsc_rate(0, 0, 59999999, 20000000) == 3000000000u - 50u);
    assert(pw_tsc_rate(0, 0, 3, 2000000000u) == 2u);
    /* A clock that stood still or ran backwards gives no rate. */
    assert(pw_tsc_rate(10, 0, 10, 20000000) == 0);
    assert(pw_tsc_rate(10, 0, 5, 20000000) == 0);
    assert(pw_tsc_rate(0, 20000000, 1000, 20000000) == 0);
    assert(pw_tsc_rate(0, 20000000, 1000, 10000000) == 0);
}

static void test_ms(void)
{
    const uint64_t ghz = 1600000000u;

    assert(pw_tsc_ms(0, ghz) == 0);
    assert(pw_tsc_ms(1599999, ghz) == 0);       /* 0.99999 ms truncates */
    assert(pw_tsc_ms(1600000, ghz) == 1);
    assert(pw_tsc_ms(5u * ghz, ghz) == 5000);   /* the report period */
    assert(pw_tsc_ms(5u * ghz + 1599999, ghz) == 5000);
    assert(pw_tsc_ms(5u * ghz + 1600000, ghz) == 5001);
    /* The docs' example window: 6405 ms at 3 GHz. */
    assert(pw_tsc_ms(19215000000ull, 3000000000u) == 6405);
    /* A whole uptime's TSC (for the process-wide native dump) does not
     * overflow: 2^64 - 1 ticks at 1.6 GHz is 11,529,215,046,068 ms. */
    assert(pw_tsc_ms(UINT64_MAX, ghz) == 11529215046068ull);
    /* No rate: never a division by zero. */
    assert(pw_tsc_ms(12345, 0) == 0);
}

/* pw_tsc_ticks(ms) is the exact boundary of pw_tsc_ms: one tick less is
 * still short of ms, so at any real TSC rate a period check on ticks fires
 * exactly when the millisecond check on the old clock would have. */
static void test_period_boundary(void)
{
    static const uint64_t periods[] = { 1, 999, 1000, 5000, 6405, 60000 };

    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
        for (unsigned p = 0; p < sizeof(periods) / sizeof(periods[0]); p++)
        {
            uint64_t ticks = pw_tsc_ticks(periods[p], rates[r]);

            assert(pw_tsc_ms(ticks, rates[r]) >= periods[p]);
            /* Exact while a tick is no longer than a millisecond. */
            if (rates[r] >= 1000u) assert(pw_tsc_ms(ticks, rates[r]) == periods[p]);
            assert(pw_tsc_ms(ticks - 1, rates[r]) < periods[p]);
        }
    assert(pw_tsc_ticks(5000, 1600000000u) == 8000000000ull);
    assert(pw_tsc_ticks(5000, 2999999999u) == 14999999995ull);
    assert(pw_tsc_ticks(1, 1000u) == 1);
    assert(pw_tsc_ticks(1, 1u) == 1);  /* 0.001 ticks rounds up */
    assert(pw_tsc_ticks(0, 1600000000u) == 0);
}

/* The report sequence profile_maybe_dump produces: a thread that returns
 * from run() every 50 us at a known rate reports once every 5000 ms, with
 * interval_ms=5000, and the windows add up to the elapsed time. */
static void test_report_sequence(void)
{
    for (unsigned r = 0; r < 3; r++)
    {
        const uint64_t rate = rates[r], step = rate / 20000u;  /* 50 us */
        const uint64_t period = pw_tsc_ticks(5000, rate);
        uint64_t tsc = 123456789u, last = 0, reported_ms = 0, reports = 0;

        for (uint64_t exit = 0; exit < 20000u * 31u; exit++, tsc += step)
        {
            if (!last) { last = tsc; continue; }
            if (tsc - last < period) continue;
            {
                uint64_t interval = pw_tsc_ms(tsc - last, rate);

                assert(interval >= 5000 && interval <= 5001);
                reported_ms += interval;
                reports++;
                last = tsc;
            }
        }
        /* 31 s of exits: six 5-second windows. */
        assert(reports == 6);
        assert(reported_ms >= 30000 && reported_ms <= 30006);
    }
}

int main(void)
{
    test_rate();
    test_ms();
    test_period_boundary();
    test_report_sequence();
    printf("test_pw_wow_tsc_clock passed\n");
    return 0;
}
