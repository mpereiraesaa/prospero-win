/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "wine/ps5/vulkan/pw_vk_present_interval.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    struct pw_vk_present_interval s = {0}, other = {0};
    uint64_t us;
    assert(!pw_vk_present_interval_add(&s, 0, 10000000, 1, &us));
    assert(pw_vk_present_interval_add(&s, 250000, 10000000, 1, &us));
    assert(us == 25000 && s.over25 == 0);
    assert(pw_vk_present_interval_add(&s, 500001, 10000000, 1, &us));
    assert(us == 25000 && s.over25 == 1 && s.over33 == 0);
    assert(pw_vk_present_interval_add(&s, 830001, 10000000, 1, &us));
    assert(us == 33000 && s.over33 == 0);
    assert(pw_vk_present_interval_add(&s, 1160002, 10000000, 1, &us));
    assert(s.over33 == 1 && s.over50 == 0);
    assert(pw_vk_present_interval_add(&s, 1660002, 10000000, 1, &us));
    assert(us == 50000 && s.over50 == 0);
    assert(pw_vk_present_interval_add(&s, 2160003, 10000000, 1, &us));
    assert(s.over50 == 1 && s.maximum_us == 50000 && s.intervals == 6);
    assert(!pw_vk_present_interval_add(&other, 500, 1000, 1, &us));
    assert(pw_vk_present_interval_add(&other, 510, 1000, 1, &us));
    assert(us == 10000 && other.intervals == 1 && s.intervals == 6);
    assert(!pw_vk_present_interval_add(&s, 9000000, 10000000, 0, &us));
    assert(!pw_vk_present_interval_add(&s, 99000000, 10000000, 1, &us));
    assert(!pw_vk_present_interval_add(&s, 50, 10000000, 1, &us));
    assert(!pw_vk_present_interval_add(&s, 60, 0, 1, &us));
    assert(!pw_vk_present_interval_add(&s, 60, UINT64_MAX, 1, &us));
    s = (struct pw_vk_present_interval){0};
    assert(!pw_vk_present_interval_add(&s, 0, 1, 1, &us));
    assert(pw_vk_present_interval_add(&s, UINT64_MAX, 1, 1, &us));
    assert(us == UINT64_MAX && s.over25 == 1 && s.over33 == 1 && s.over50 == 1);
    s.intervals = s.over25 = s.over33 = s.over50 = UINT64_MAX;
    s.previous = 0;
    assert(pw_vk_present_interval_add(&s, 2, 1, 1, &us));
    assert(s.intervals == UINT64_MAX && s.over50 == UINT64_MAX);
    puts("Present interval accumulator: PASS");
    return 0;
}
