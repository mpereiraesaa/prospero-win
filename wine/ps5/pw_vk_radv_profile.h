/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_RADV_PROFILE_H
#define PW_VK_RADV_PROFILE_H
#include <stddef.h>
#include <stdint.h>

/* Where RADV's time goes, by entry point and by frame, on every thread that
 * calls it. libvulkan.prx's loader shim (pw_vulkan_radv.c) hands Wine a
 * timing wrapper for each entry point in pw_vk_radv_profile_list.h when the
 * profile is on; each wrapper adds the call's TSC ticks to the calling
 * thread's table. vkQueuePresentKHR ends a frame: it logs a present line
 * with the interval since the last one, and the first wrapped call a thread
 * makes after a present logs that thread's table for the frames it has just
 * seen end (its top entries by time, the rest folded into other), then calls
 * the frame hook wow64native installs (wine/wow64native/unix.c), which logs
 * the thread's host/guest split for the same interval. Calls over an event
 * threshold log their own line, with a detail for creation, allocation and
 * waiting functions. Each line stays under a ps5log record.
 *
 * This file is the pure, host-testable part: the tables and the lines. */

#define PW_VK_RADV_FN(name, ret, params, args) PW_VK_RADV_##name,
#define PW_VK_RADV_FN_VOID(name, params, args) PW_VK_RADV_##name,
#define PW_VK_RADV_FN_HAND(name, ret, params, args) PW_VK_RADV_##name,
enum pw_vk_radv_function {
#include "pw_vk_radv_profile_list.h"
    PW_VK_RADV_FUNCTION_COUNT
};
#undef PW_VK_RADV_FN
#undef PW_VK_RADV_FN_VOID
#undef PW_VK_RADV_FN_HAND

enum {
    PW_VK_RADV_PROFILE_VERSION = 1,
    PW_VK_RADV_PROFILE_TOP = 12,       /* entries named in a frame line */
    PW_VK_RADV_PROFILE_LINE = 1000,    /* a ps5log record holds 1024 bytes */
    PW_VK_RADV_PROFILE_DETAIL = 200,
};

extern const char *const pw_vk_radv_profile_names[PW_VK_RADV_FUNCTION_COUNT];

/* One thread's table: the counters since its last rollover. */
struct pw_vk_radv_profile_thread {
    uint64_t frame;        /* the present count at the last rollover */
    uint64_t since_tsc;    /* when the counters started */
    uint32_t index;        /* the thread's number in the log */
    uint64_t calls[PW_VK_RADV_FUNCTION_COUNT], ticks[PW_VK_RADV_FUNCTION_COUNT];
};

static inline void pw_vk_radv_profile_add(struct pw_vk_radv_profile_thread *t, unsigned fn, uint64_t ticks)
{
    t->calls[fn]++;
    t->ticks[fn] += ticks;
}

/* Ends the thread's interval at present count frame and TSC tsc: formats its
 * line into out (cap bytes) when it had any call, clears the counters and
 * starts the next interval. Returns the line's length, 0 for no line. */
size_t pw_vk_radv_profile_rollover(struct pw_vk_radv_profile_thread *t, uint64_t frame, uint64_t tsc,
                                   char *out, size_t cap);
/* The line for one slow call: fn's id and name, its ticks and an optional
 * detail (argument values, already formatted). */
size_t pw_vk_radv_profile_format_event(char *out, size_t cap, uint32_t thread, uint64_t frame, uint64_t tsc,
                                       unsigned fn, uint64_t ticks, const char *detail);
/* The line for one present: its number, the interval since the previous
 * present's return, the call's own ticks and its VkResult. */
size_t pw_vk_radv_profile_format_present(char *out, size_t cap, uint32_t thread, uint64_t frame, uint64_t tsc,
                                         uint64_t interval, uint64_t ticks, int result);
/* The line written once when the profile starts. */
size_t pw_vk_radv_profile_format_start(char *out, size_t cap, uint64_t tsc_hz, uint64_t event_us, int from_env);

#endif
