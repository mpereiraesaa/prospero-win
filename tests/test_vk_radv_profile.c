/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The RADV entry-point profile's tables and lines (wine/ps5/pw_vk_radv_profile.h). */
#include "pw_vk_radv_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int count_token(const char *line, const char *token)
{
    int count = 0;
    size_t length = strlen(token);
    for (const char *at = strstr(line, token); at; at = strstr(at + length, token)) count++;
    return count;
}

int main(void)
{
    struct pw_vk_radv_profile_thread t = {0};
    char line[PW_VK_RADV_PROFILE_LINE], small[128];
    size_t n;
    unsigned fn;

    assert((unsigned)PW_VK_RADV_FUNCTION_COUNT > (unsigned)PW_VK_RADV_PROFILE_TOP);
    assert(!strcmp(pw_vk_radv_profile_names[PW_VK_RADV_vkCmdDraw], "vkCmdDraw"));
    assert(!strcmp(pw_vk_radv_profile_names[PW_VK_RADV_vkQueuePresentKHR], "vkQueuePresentKHR"));
    for (fn = 0; fn < PW_VK_RADV_FUNCTION_COUNT; fn++) assert(strlen(pw_vk_radv_profile_names[fn]) < 48);

    /* No calls: no line, but the interval moves on. */
    t.index = 3;
    t.frame = 10;
    t.since_tsc = 1000;
    n = pw_vk_radv_profile_rollover(&t, 11, 2000, line, sizeof(line));
    assert(n == 0 && t.frame == 11 && t.since_tsc == 2000);

    /* Two entries, the busier first; frames and wall from the last rollover. */
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdDraw, 100);
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdDraw, 50);
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdBindPipeline, 400);
    n = pw_vk_radv_profile_rollover(&t, 13, 5000, line, sizeof(line));
    assert(n == strlen(line) && n > 0);
    assert(!strcmp(line, "PW_VK_RADV_PROFILE version=1 frame=13 frames=2 thread=3 tsc=5000 wall=3000 calls=3 "
                         "ticks=550 vkCmdBindPipeline=1/400 vkCmdDraw=2/150"));
    assert(t.frame == 13 && t.since_tsc == 5000 && t.calls[PW_VK_RADV_vkCmdDraw] == 0 && t.ticks[PW_VK_RADV_vkCmdDraw] == 0);

    /* More entries than the line names: the rest are folded into other,
     * the named ones are the ones with the most time, ties in id order. */
    for (fn = 0; fn < PW_VK_RADV_PROFILE_TOP + 5; fn++) pw_vk_radv_profile_add(&t, fn, 10 + (fn % 3));
    pw_vk_radv_profile_add(&t, PW_VK_RADV_FUNCTION_COUNT - 1, 1000);
    n = pw_vk_radv_profile_rollover(&t, 14, 6000, line, sizeof(line));
    assert(n > 0 && count_token(line, "=1/") == PW_VK_RADV_PROFILE_TOP);
    assert(strstr(line, "frames=1 thread=3 tsc=6000 wall=1000 calls=18 ticks="));
    assert(strstr(line, " other=") && strstr(line, pw_vk_radv_profile_names[PW_VK_RADV_FUNCTION_COUNT - 1]));
    {
        /* The last named entry comes right after the function with 1000 ticks and the 12-tick ones. */
        const char *first = strstr(line, pw_vk_radv_profile_names[PW_VK_RADV_FUNCTION_COUNT - 1]);
        const char *other = strstr(line, " other=");
        assert(first && other && first < other);
        assert(strstr(line, " other=6/"));
    }
    /* A cap that holds the header but not every entry keeps whole entries;
     * one too small for the header writes no line at all. */
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdDraw, 7);
    n = pw_vk_radv_profile_rollover(&t, 15, 6500, small, 100);
    assert(n == strlen(small) && n > 0 && !strcmp(small, "PW_VK_RADV_PROFILE version=1 frame=15 frames=1 thread=3 "
                                                         "tsc=6500 wall=500 calls=1 ticks=7"));
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdDraw, 7);
    small[0] = 'x';
    n = pw_vk_radv_profile_rollover(&t, 16, 6600, small, 40);
    assert(n == 0 && small[0] == 0 && t.frame == 16);
    /* A zero cap writes nothing and still resets. */
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdDraw, 7);
    assert(pw_vk_radv_profile_rollover(&t, 17, 7000, line, 0) == 0 && t.calls[PW_VK_RADV_vkCmdDraw] == 0);
    /* A frame or clock that did not move reports 0, not a wrapped value. */
    pw_vk_radv_profile_add(&t, PW_VK_RADV_vkCmdDraw, 7);
    n = pw_vk_radv_profile_rollover(&t, 17, 6000, line, sizeof(line));
    assert(strstr(line, " frames=0 ") && strstr(line, " wall=0 "));

    n = pw_vk_radv_profile_format_event(line, sizeof(line), 2, 40, 123456, PW_VK_RADV_vkCreateGraphicsPipelines,
                                        9000, "count=1 flags=0x800");
    assert(n == strlen(line));
    assert(!strcmp(line, "PW_VK_RADV_EVENT version=1 frame=40 thread=2 tsc=123456 fn=vkCreateGraphicsPipelines "
                         "ticks=9000 count=1 flags=0x800"));
    n = pw_vk_radv_profile_format_event(line, sizeof(line), 2, 40, 1, PW_VK_RADV_vkCmdDraw, 5, NULL);
    assert(!strcmp(line, "PW_VK_RADV_EVENT version=1 frame=40 thread=2 tsc=1 fn=vkCmdDraw ticks=5"));
    assert(pw_vk_radv_profile_format_event(line, sizeof(line), 2, 40, 1, PW_VK_RADV_FUNCTION_COUNT, 5, "") == 0);

    n = pw_vk_radv_profile_format_present(line, sizeof(line), 1, 41, 200000, 16700, 300, 0);
    assert(!strcmp(line, "PW_VK_RADV_PRESENT version=1 frame=41 thread=1 tsc=200000 interval=16700 ticks=300 result=0"));
    n = pw_vk_radv_profile_format_present(line, sizeof(line), 1, 42, 300000, 0, 300, 1000001003);
    assert(strstr(line, " interval=0 ") && strstr(line, "result=1000001003"));

    n = pw_vk_radv_profile_format_start(line, sizeof(line), 3500000000ull, 200, 1);
    assert(strstr(line, "PW_VK_RADV_PROFILE version=1 start tsc_hz=3500000000 event_us=200 functions="));
    assert(strstr(line, " enabled_by=env"));
    n = pw_vk_radv_profile_format_start(line, sizeof(line), 0, 50, 0);
    assert(strstr(line, "tsc_hz=0 event_us=50") && strstr(line, "enabled_by=hook"));

    puts("RADV entry-point profile: PASS");
    return 0;
}
