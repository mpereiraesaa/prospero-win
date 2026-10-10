/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_TSC_CALIBRATE_H
#define PW_TSC_CALIBRATE_H
/*
 * The TSC frequency for [runtime] fast_clock (patch 0900's PW_QPC_TSC_HZ).
 * The title measures it twice against CLOCK_MONOTONIC, each time over about
 * 25 ms, and trusts it only when both measurements agree within 0.05% and
 * the result lies in the range patch 0900 accepts. Each end of a window is
 * a TSC read bracketed by two clock reads, so a preemption between them
 * shows up as a wide bracket and refuses the measurement instead of skewing
 * it. Any refusal leaves the game on Wine's normal counter.
 */
#include <stdint.h>

#define PW_TSC_MIN_HZ UINT64_C(20000000)        /* PW_QPC_MIN_TSC_HZ */
#define PW_TSC_MAX_HZ UINT64_C(10000000000)     /* PW_QPC_MAX_TSC_HZ */
enum {
    PW_TSC_WINDOW_NS = 25000000,        /* each measurement */
    PW_TSC_MAX_BRACKET_NS = 20000,      /* clock reads around one TSC read */
    PW_TSC_AGREE_PPM = 500,             /* 0.05% between the two */
};

/* One end of a window: clock, TSC, clock. */
typedef struct PwTscPoint { uint64_t before_ns, tsc, after_ns; } PwTscPoint;

typedef enum PwTscStatus {
    PW_TSC_OK = 0,
    PW_TSC_BRACKET,     /* an end's clock reads were too far apart */
    PW_TSC_SHORT,       /* the window was shorter than PW_TSC_WINDOW_NS */
    PW_TSC_BACKWARD,    /* TSC or clock went backwards */
    PW_TSC_RANGE,       /* outside PW_TSC_MIN_HZ..PW_TSC_MAX_HZ */
    PW_TSC_DISAGREE,    /* the two windows differ by more than 0.05% */
} PwTscStatus;

/* The frequency over one window, in Hz. */
PwTscStatus pw_tsc_window_hz(const PwTscPoint *begin, const PwTscPoint *end, uint64_t *hz);
/* Both windows' frequencies, and their mean when they agree. */
PwTscStatus pw_tsc_calibrate(const PwTscPoint window[4], uint64_t *hz);
/* Takes the four points with the given readers, waiting out each window
 * by reading the clock, then calibrates. */
PwTscStatus pw_tsc_measure(uint64_t (*clock_ns)(void *), uint64_t (*tsc)(void *), void *context,
                           uint64_t *hz);
const char *pw_tsc_status_name(PwTscStatus status);

#endif
