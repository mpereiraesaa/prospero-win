/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_PRESENT_INTERVAL_H
#define PW_VK_PRESENT_INTERVAL_H
#include <stdint.h>
/* Caller serializes one accumulator per queue. Timestamps are QPC ticks at
 * successful vkQueuePresentKHR returns, not scanout or GPU completion times. */
struct pw_vk_present_interval {
    uint64_t previous, intervals, maximum_us, over25, over33, over50;
    int primed;
};
static inline int pw_vk_present_interval_add(struct pw_vk_present_interval *s,
    uint64_t now, uint64_t frequency, int success, uint64_t *interval_us)
{
    uint64_t delta, seconds, us;
    *interval_us = 0;
    if (!success || !frequency || frequency > UINT64_MAX / 1000000) {
        s->primed = 0;
        return 0;
    }
    if (!s->primed || now < s->previous) {
        s->previous = now; s->primed = 1;
        return 0;
    }
    delta = now - s->previous; s->previous = now;
    seconds = delta / frequency;
    if (seconds > (UINT64_MAX - 999999) / 1000000) us = UINT64_MAX;
    else us = seconds * 1000000 + (delta % frequency) * 1000000 / frequency;
    *interval_us = us;
    if (s->intervals != UINT64_MAX) ++s->intervals;
    if (us > s->maximum_us) s->maximum_us = us;
    /* Thresholds are strict, using ticks to avoid rounding away sub-us excess. */
    if ((seconds || delta > frequency * 25 / 1000) && s->over25 != UINT64_MAX) ++s->over25;
    if ((seconds || delta > frequency * 33 / 1000) && s->over33 != UINT64_MAX) ++s->over33;
    if ((seconds || delta > frequency * 50 / 1000) && s->over50 != UINT64_MAX) ++s->over50;
    return 1;
}
#endif
