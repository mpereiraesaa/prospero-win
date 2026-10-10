/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_radv_profile.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PW_VK_RADV_FN(name, ret, params, args) #name,
#define PW_VK_RADV_FN_VOID(name, params, args) #name,
#define PW_VK_RADV_FN_HAND(name, ret, params, args) #name,
const char *const pw_vk_radv_profile_names[PW_VK_RADV_FUNCTION_COUNT] = {
#include "pw_vk_radv_profile_list.h"
};
#undef PW_VK_RADV_FN
#undef PW_VK_RADV_FN_VOID
#undef PW_VK_RADV_FN_HAND

/* Appends formatted text, never past cap; returns the new length. A line
 * that would overflow is cut at the last whole entry. */
static size_t append(char *out, size_t cap, size_t used, const char *format, ...)
{
    va_list args;
    int n;
    if (used >= cap) return used;
    va_start(args, format);
    n = vsnprintf(out + used, cap - used, format, args);
    va_end(args);
    if (n < 0) return used;
    if ((size_t)n >= cap - used)
    {
        out[used] = 0;
        return used;
    }
    return used + (size_t)n;
}

size_t pw_vk_radv_profile_rollover(struct pw_vk_radv_profile_thread *t, uint64_t frame, uint64_t tsc,
                                   char *out, size_t cap)
{
    uint64_t calls = 0, ticks = 0, other_calls = 0, other_ticks = 0;
    unsigned used_fns[PW_VK_RADV_FUNCTION_COUNT], count = 0, fn, i, j;
    size_t used = 0;

    for (fn = 0; fn < PW_VK_RADV_FUNCTION_COUNT; fn++)
    {
        if (!t->calls[fn]) continue;
        calls += t->calls[fn];
        ticks += t->ticks[fn];
        /* Insertion sort by ticks, most first; ties keep the id order. */
        for (i = count; i > 0 && t->ticks[used_fns[i - 1]] < t->ticks[fn]; i--) used_fns[i] = used_fns[i - 1];
        used_fns[i] = fn;
        count++;
    }
    if (calls && cap)
    {
        used = append(out, cap, 0, "PW_VK_RADV_PROFILE version=%d frame=%llu frames=%llu thread=%u tsc=%llu wall=%llu "
                      "calls=%llu ticks=%llu", PW_VK_RADV_PROFILE_VERSION, (unsigned long long)frame,
                      (unsigned long long)(frame > t->frame ? frame - t->frame : 0), t->index,
                      (unsigned long long)tsc, (unsigned long long)(tsc > t->since_tsc ? tsc - t->since_tsc : 0),
                      (unsigned long long)calls, (unsigned long long)ticks);
        /* No header, no line: the entries alone would not say which frame. */
        if (!used) count = 0;
        for (j = 0; j < count && j < PW_VK_RADV_PROFILE_TOP; j++)
            used = append(out, cap, used, " %s=%llu/%llu", pw_vk_radv_profile_names[used_fns[j]],
                          (unsigned long long)t->calls[used_fns[j]], (unsigned long long)t->ticks[used_fns[j]]);
        for (; j < count; j++)
        {
            other_calls += t->calls[used_fns[j]];
            other_ticks += t->ticks[used_fns[j]];
        }
        if (other_calls)
            used = append(out, cap, used, " other=%llu/%llu", (unsigned long long)other_calls,
                          (unsigned long long)other_ticks);
    }
    memset(t->calls, 0, sizeof(t->calls));
    memset(t->ticks, 0, sizeof(t->ticks));
    t->frame = frame;
    t->since_tsc = tsc;
    return used;
}

size_t pw_vk_radv_profile_format_event(char *out, size_t cap, uint32_t thread, uint64_t frame, uint64_t tsc,
                                       unsigned fn, uint64_t ticks, const char *detail)
{
    size_t used;
    if (fn >= PW_VK_RADV_FUNCTION_COUNT) return 0;
    used = append(out, cap, 0, "PW_VK_RADV_EVENT version=%d frame=%llu thread=%u tsc=%llu fn=%s ticks=%llu",
                  PW_VK_RADV_PROFILE_VERSION, (unsigned long long)frame, thread, (unsigned long long)tsc,
                  pw_vk_radv_profile_names[fn], (unsigned long long)ticks);
    if (detail && *detail) used = append(out, cap, used, " %s", detail);
    return used;
}

size_t pw_vk_radv_profile_format_present(char *out, size_t cap, uint32_t thread, uint64_t frame, uint64_t tsc,
                                         uint64_t interval, uint64_t ticks, int result)
{
    return append(out, cap, 0, "PW_VK_RADV_PRESENT version=%d frame=%llu thread=%u tsc=%llu interval=%llu "
                  "ticks=%llu result=%d", PW_VK_RADV_PROFILE_VERSION, (unsigned long long)frame, thread,
                  (unsigned long long)tsc, (unsigned long long)interval, (unsigned long long)ticks, result);
}

size_t pw_vk_radv_profile_format_start(char *out, size_t cap, uint64_t tsc_hz, uint64_t event_us, int from_env)
{
    return append(out, cap, 0, "PW_VK_RADV_PROFILE version=%d start tsc_hz=%llu event_us=%llu functions=%u "
                  "enabled_by=%s", PW_VK_RADV_PROFILE_VERSION, (unsigned long long)tsc_hz,
                  (unsigned long long)event_us, (unsigned)PW_VK_RADV_FUNCTION_COUNT, from_env ? "env" : "hook");
}
