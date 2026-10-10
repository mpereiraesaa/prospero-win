/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_tsc_calibrate.h"

PwTscStatus pw_tsc_window_hz(const PwTscPoint *begin, const PwTscPoint *end, uint64_t *hz)
{
    uint64_t start_ns, stop_ns, elapsed_ns, ticks;
    unsigned __int128 scaled;

    if (begin->after_ns < begin->before_ns || end->after_ns < end->before_ns) return PW_TSC_BACKWARD;
    if (begin->after_ns - begin->before_ns > PW_TSC_MAX_BRACKET_NS ||
        end->after_ns - end->before_ns > PW_TSC_MAX_BRACKET_NS) return PW_TSC_BRACKET;
    /* each TSC read is taken at the middle of its bracket */
    start_ns = begin->before_ns + (begin->after_ns - begin->before_ns) / 2;
    stop_ns = end->before_ns + (end->after_ns - end->before_ns) / 2;
    if (stop_ns <= start_ns || end->tsc <= begin->tsc) return PW_TSC_BACKWARD;
    elapsed_ns = stop_ns - start_ns;
    if (elapsed_ns < PW_TSC_WINDOW_NS) return PW_TSC_SHORT;
    ticks = end->tsc - begin->tsc;
    scaled = (unsigned __int128)ticks * 1000000000u + elapsed_ns / 2;
    scaled /= elapsed_ns;
    if (scaled < PW_TSC_MIN_HZ || scaled > PW_TSC_MAX_HZ) return PW_TSC_RANGE;
    *hz = (uint64_t)scaled;
    return PW_TSC_OK;
}

PwTscStatus pw_tsc_calibrate(const PwTscPoint window[4], uint64_t *hz)
{
    uint64_t first, second, low, high;
    PwTscStatus status;

    if ((status = pw_tsc_window_hz(&window[0], &window[1], &first)) != PW_TSC_OK) return status;
    if ((status = pw_tsc_window_hz(&window[2], &window[3], &second)) != PW_TSC_OK) return status;
    low = first < second ? first : second;
    high = first < second ? second : first;
    /* (high - low) / low <= 500 ppm; low >= 20 MHz keeps this in range */
    if ((high - low) * 1000000u > low * (uint64_t)PW_TSC_AGREE_PPM) return PW_TSC_DISAGREE;
    *hz = low + (high - low) / 2;
    return PW_TSC_OK;
}

static void take(PwTscPoint *point, uint64_t (*clock_ns)(void *), uint64_t (*tsc)(void *), void *context)
{
    point->before_ns = clock_ns(context);
    point->tsc = tsc(context);
    point->after_ns = clock_ns(context);
}

PwTscStatus pw_tsc_measure(uint64_t (*clock_ns)(void *), uint64_t (*tsc)(void *), void *context,
                           uint64_t *hz)
{
    PwTscPoint window[4];

    for (int w = 0; w < 2; w++) {
        take(&window[2 * w], clock_ns, tsc, context);
        /* a little past the window, so the bracket midpoints span it */
        while (clock_ns(context) - window[2 * w].after_ns < PW_TSC_WINDOW_NS + PW_TSC_MAX_BRACKET_NS) {}
        take(&window[2 * w + 1], clock_ns, tsc, context);
    }
    return pw_tsc_calibrate(window, hz);
}

const char *pw_tsc_status_name(PwTscStatus status)
{
    switch (status) {
    case PW_TSC_OK: return "ok";
    case PW_TSC_BRACKET: return "bracket";
    case PW_TSC_SHORT: return "short";
    case PW_TSC_BACKWARD: return "backward";
    case PW_TSC_RANGE: return "range";
    case PW_TSC_DISAGREE: return "disagree";
    }
    return "unknown";
}
