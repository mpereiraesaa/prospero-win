/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_tsc_calibrate.h"
#include <assert.h>
#include <stdio.h>

/* A simulated machine: every clock read advances the clock by step_ns, and
 * the TSC runs at hz (hz2 from the second window on), anchored to the
 * clock. stall_ns is added once, on the read numbered stall_at. */
typedef struct Sim {
    uint64_t now_ns, step_ns, hz, hz2, stall_ns;
    unsigned reads, stall_at, windows_started;
    int backward_tsc;
} Sim;

static uint64_t sim_clock(void *context)
{
    Sim *sim = context;
    if (++sim->reads == sim->stall_at) sim->now_ns += sim->stall_ns;
    return sim->now_ns += sim->step_ns;
}

static uint64_t sim_tsc(void *context)
{
    Sim *sim = context;
    uint64_t hz = sim->windows_started++ >= 2 && sim->hz2 ? sim->hz2 : sim->hz;
    if (sim->backward_tsc && sim->windows_started == 2) return 1;
    return (uint64_t)((unsigned __int128)sim->now_ns * hz / 1000000000u);
}

static PwTscStatus measure(Sim sim, uint64_t *hz)
{
    return pw_tsc_measure(sim_clock, sim_tsc, &sim, hz);
}

static PwTscPoint at(uint64_t ns, uint64_t tsc) { return (PwTscPoint){ ns, tsc, ns + 100 }; }

int main(void)
{
    uint64_t hz = 0;

    /* a steady 1.6 GHz TSC, measured exactly */
    assert(measure((Sim){ .step_ns = 1000, .hz = 1600000000u }, &hz) == PW_TSC_OK);
    assert(hz >= 1599999000u && hz <= 1600001000u);
    /* the range bounds of patch 0900 */
    assert(measure((Sim){ .step_ns = 1000, .hz = 20000000u }, &hz) == PW_TSC_OK);
    assert(measure((Sim){ .step_ns = 1000, .hz = 10000000000u }, &hz) == PW_TSC_OK);
    assert(measure((Sim){ .step_ns = 1000, .hz = 19000000u }, &hz) == PW_TSC_RANGE);
    assert(measure((Sim){ .step_ns = 1000, .hz = 11000000000u }, &hz) == PW_TSC_RANGE);

    /* two windows 0.04% apart agree; 0.06% apart do not */
    hz = 0;
    assert(measure((Sim){ .step_ns = 1000, .hz = 1600000000u, .hz2 = 1600640000u }, &hz) == PW_TSC_OK);
    assert(hz > 1600000000u && hz < 1600640000u);
    assert(measure((Sim){ .step_ns = 1000, .hz = 1600000000u, .hz2 = 1600960000u }, &hz) == PW_TSC_DISAGREE);

    /* a stall between a TSC read and its clock reads refuses the window */
    assert(measure((Sim){ .step_ns = 1000, .hz = 1600000000u, .stall_at = 2, .stall_ns = 50000 }, &hz)
           == PW_TSC_BRACKET);
    /* a TSC that goes backwards */
    assert(measure((Sim){ .step_ns = 1000, .hz = 1600000000u, .backward_tsc = 1 }, &hz) == PW_TSC_BACKWARD);

    /* the window math on its own */
    {
        PwTscPoint short_end = at(10000000, 16000000), begin = at(0, 0), end = at(25000000, 40000000);
        PwTscPoint wide = { 25000000, 40000000, 25000000 + PW_TSC_MAX_BRACKET_NS + 1 };
        PwTscPoint reversed = { 100, 0, 50 };
        assert(pw_tsc_window_hz(&begin, &end, &hz) == PW_TSC_OK && hz == 1600000000u);
        assert(pw_tsc_window_hz(&begin, &short_end, &hz) == PW_TSC_SHORT);
        assert(pw_tsc_window_hz(&begin, &wide, &hz) == PW_TSC_BRACKET);
        assert(pw_tsc_window_hz(&reversed, &end, &hz) == PW_TSC_BACKWARD);
        assert(pw_tsc_window_hz(&end, &begin, &hz) == PW_TSC_BACKWARD);
    }
    assert(pw_tsc_status_name(PW_TSC_DISAGREE)[0] == 'd');

    printf("TSC calibration passed: exact, range bounds, agreement within 0.05%%, stalls and backwards "
           "readings refused\n");
    return 0;
}
