/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The timing wrappers libvulkan.prx hands Wine in place of RADV's entry
 * points while the profile is on (pw_vk_radv_profile.h): each times the
 * real call with the TSC and files it in the calling thread's table.
 * Linked into libvulkan.prx by tools/link_radv_prx.sh. */
#define _POSIX_C_SOURCE 200809L
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>
#include "pw_vk_radv_profile.h"
#include "pw_vk_radv_profile_wrap.h"

static struct {
    int initialized, enabled, from_env;
    uint64_t tsc_hz, event_ticks;
    _Atomic uint64_t presents;
    _Atomic uint64_t last_present_tsc;
    _Atomic uint32_t threads;
    pw_vk_radv_frame_hook hook;
    pthread_key_t key;
    pthread_mutex_t lock;
} state = { .lock = PTHREAD_MUTEX_INITIALIZER };
static PFN_vkVoidFunction real[PW_VK_RADV_FUNCTION_COUNT];

static void emit(const char *line, size_t length)
{
    if (length) fprintf(stderr, "%s\n", line);
}

/* PW_QPC_TSC_HZ when the title measured it, else 20 ms against
 * CLOCK_MONOTONIC, as wine/wow64native/unix.c does. */
static uint64_t measure_tsc_hz(void)
{
    const char *text = getenv("PW_QPC_TSC_HZ");
    struct timespec start, now;
    uint64_t tsc0, tsc1, ns;
    if (text && *text)
    {
        char *end;
        unsigned long long hz = strtoull(text, &end, 10);
        if (!*end && hz >= 100000000ull && hz <= 10000000000ull) return hz;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 0;
    tsc0 = __rdtsc();
    do
    {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
        ns = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull + (uint64_t)now.tv_nsec - (uint64_t)start.tv_nsec;
    } while (ns < 20000000ull);
    tsc1 = __rdtsc();
    return tsc1 > tsc0 ? (tsc1 - tsc0) * 1000000000ull / ns : 0;
}

static int env_is_one(const char *name)
{
    const char *value = getenv(name);
    return value && !strcmp(value, "1");
}

/* Under the lock. Turns the profile on once: the clock, the event
 * threshold (PW_VK_RADV_EVENT_US, 200 us by default) and the thread key. */
static void enable_locked(int from_env)
{
    char line[PW_VK_RADV_PROFILE_LINE];
    const char *text = getenv("PW_VK_RADV_EVENT_US");
    uint64_t event_us = 200;
    if (state.enabled) return;
    if (text && *text)
    {
        char *end;
        unsigned long long us = strtoull(text, &end, 10);
        if (!*end && us <= 1000000ull) event_us = us;
    }
    if (pthread_key_create(&state.key, free)) return;
    state.tsc_hz = measure_tsc_hz();
    state.event_ticks = state.tsc_hz ? event_us * state.tsc_hz / 1000000ull : ~0ull;
    state.from_env = from_env;
    state.enabled = 1;
    emit(line, pw_vk_radv_profile_format_start(line, sizeof(line), state.tsc_hz, event_us, from_env));
    /* Where Mesa would keep its shader cache: without one every run
     * compiles every pipeline again, which is what a cold first minute
     * looks like. The values are the environment's, unset ones say so. */
    {
        static const char *const names[] = { "MESA_SHADER_CACHE_DIR", "MESA_SHADER_CACHE_DISABLE",
                                             "MESA_SHADER_CACHE_MAX_SIZE", "XDG_CACHE_HOME", "HOME" };
        size_t used = (size_t)snprintf(line, sizeof(line), "PW_VK_RADV_PROFILE version=%d env", PW_VK_RADV_PROFILE_VERSION);
        const char *root = getenv("MESA_SHADER_CACHE_DIR"), *home = getenv("HOME"), *xdg = getenv("XDG_CACHE_HOME");
        char probe[512];
        int n;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && used < sizeof(line); i++)
        {
            const char *value = getenv(names[i]);
            n = snprintf(line + used, sizeof(line) - used, " %s=%s", names[i], value ? value : "(unset)");
            if (n < 0 || (size_t)n >= sizeof(line) - used) break;
            used += (size_t)n;
        }
        /* The root Mesa derives its cache directory from, and whether this
         * process could write there: the same order Mesa uses. */
        if (root && *root) snprintf(probe, sizeof(probe), "%s", root);
        else if (xdg && *xdg) snprintf(probe, sizeof(probe), "%s", xdg);
        else if (home && *home) snprintf(probe, sizeof(probe), "%s/.cache", home);
        else probe[0] = 0;
        n = snprintf(line + used, sizeof(line) - used, " cache_root=%s writable=%d", probe[0] ? probe : "(none)",
                     probe[0] ? access(probe, W_OK) == 0 : 0);
        if (n > 0 && (size_t)n < sizeof(line) - used) used += (size_t)n;
        emit(line, used);
    }
}

static void initialize_locked(void)
{
    if (state.initialized) return;
    state.initialized = 1;
    if (env_is_one("PW_VK_RADV_PROFILE") || env_is_one("PW_NATIVE_PROFILE")) enable_locked(1);
}

void pw_vk_radv_profile_set_frame_hook(pw_vk_radv_frame_hook hook)
{
    pthread_mutex_lock(&state.lock);
    initialize_locked();
    state.hook = hook;
    if (hook) enable_locked(0);
    pthread_mutex_unlock(&state.lock);
}

/* This thread's table, made on its first wrapped call. */
static struct pw_vk_radv_profile_thread *thread_table(void)
{
    struct pw_vk_radv_profile_thread *t = pthread_getspecific(state.key);
    if (!t)
    {
        t = calloc(1, sizeof(*t));
        if (!t) return NULL;
        t->index = atomic_fetch_add(&state.threads, 1) + 1;
        t->frame = atomic_load(&state.presents);
        t->since_tsc = __rdtsc();
        pthread_setspecific(state.key, t);
    }
    return t;
}

/* Ends the thread's interval when presents moved past its frame. */
static void rollover(struct pw_vk_radv_profile_thread *t, uint64_t tsc)
{
    char line[PW_VK_RADV_PROFILE_LINE];
    uint64_t frame = atomic_load(&state.presents);
    if (frame == t->frame) return;
    emit(line, pw_vk_radv_profile_rollover(t, frame, tsc, line, sizeof(line)));
    if (state.hook) state.hook(frame, tsc);
}

static void event(const struct pw_vk_radv_profile_thread *t, uint64_t tsc, unsigned fn, uint64_t ticks,
                  const char *detail)
{
    char line[PW_VK_RADV_PROFILE_LINE];
    emit(line, pw_vk_radv_profile_format_event(line, sizeof(line), t->index, atomic_load(&state.presents), tsc, fn,
                                               ticks, detail));
}

/* Files one call; detail is only read when the call was slow. */
static void account(unsigned fn, uint64_t t0, uint64_t t1, const char *detail)
{
    struct pw_vk_radv_profile_thread *t = thread_table();
    uint64_t ticks = t1 - t0;
    if (!t) return;
    rollover(t, t1);
    pw_vk_radv_profile_add(t, fn, ticks);
    if (ticks >= state.event_ticks) event(t, t1, fn, ticks, detail);
}

#define PW_VK_RADV_FN(name, ret, params, args) \
    static ret VKAPI_CALL wrap_##name params \
    { \
        uint64_t t0 = __rdtsc(), t1; \
        ret result = ((PFN_##name)real[PW_VK_RADV_##name]) args; \
        t1 = __rdtsc(); \
        account(PW_VK_RADV_##name, t0, t1, NULL); \
        return result; \
    }
#define PW_VK_RADV_FN_VOID(name, params, args) \
    static void VKAPI_CALL wrap_##name params \
    { \
        uint64_t t0 = __rdtsc(), t1; \
        ((PFN_##name)real[PW_VK_RADV_##name]) args; \
        t1 = __rdtsc(); \
        account(PW_VK_RADV_##name, t0, t1, NULL); \
    }
#define PW_VK_RADV_FN_HAND(name, ret, params, args) static ret VKAPI_CALL wrap_##name params;
#include "pw_vk_radv_profile_list.h"
#undef PW_VK_RADV_FN
#undef PW_VK_RADV_FN_VOID
#undef PW_VK_RADV_FN_HAND

/* The hand-written wrappers: the same timing, plus a detail for the event
 * line. A timed call that creates or waits is the kind of one-off cost a
 * frame spike comes from, so its arguments are worth the line. */
#define REAL(name) ((PFN_##name)real[PW_VK_RADV_##name])
#define TIMED(name, call, detail) \
    uint64_t t0 = __rdtsc(), t1; \
    VkResult result = call; \
    t1 = __rdtsc(); \
    account(PW_VK_RADV_##name, t0, t1, detail); \
    return result

static const char *pipeline_detail(char *buffer, uint32_t count, VkPipelineCreateFlags flags)
{
    snprintf(buffer, PW_VK_RADV_PROFILE_DETAIL, "count=%u flags=%#x library=%d link_time_opt=%d fail_on_compile=%d",
             count, flags, !!(flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR),
             !!(flags & VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT),
             !!(flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT));
    return buffer;
}

static VkResult VKAPI_CALL wrap_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                          uint32_t createInfoCount,
                                                          const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                          const VkAllocationCallbacks *pAllocator,
                                                          VkPipeline *pPipelines)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    TIMED(vkCreateGraphicsPipelines,
          REAL(vkCreateGraphicsPipelines)(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines),
          pipeline_detail(detail, createInfoCount, createInfoCount && pCreateInfos ? pCreateInfos[0].flags : 0));
}

static VkResult VKAPI_CALL wrap_vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                         uint32_t createInfoCount,
                                                         const VkComputePipelineCreateInfo *pCreateInfos,
                                                         const VkAllocationCallbacks *pAllocator,
                                                         VkPipeline *pPipelines)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    TIMED(vkCreateComputePipelines,
          REAL(vkCreateComputePipelines)(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines),
          pipeline_detail(detail, createInfoCount, createInfoCount && pCreateInfos ? pCreateInfos[0].flags : 0));
}

static VkResult VKAPI_CALL wrap_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator,
                                                     VkShaderModule *pShaderModule)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "code_bytes=%zu", pCreateInfo ? pCreateInfo->codeSize : (size_t)0);
    TIMED(vkCreateShaderModule, REAL(vkCreateShaderModule)(device, pCreateInfo, pAllocator, pShaderModule), detail);
}

static VkResult VKAPI_CALL wrap_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                                                 const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "bytes=%llu memory_type=%u",
             pAllocateInfo ? (unsigned long long)pAllocateInfo->allocationSize : 0ull,
             pAllocateInfo ? pAllocateInfo->memoryTypeIndex : 0u);
    TIMED(vkAllocateMemory, REAL(vkAllocateMemory)(device, pAllocateInfo, pAllocator, pMemory), detail);
}

static VkResult VKAPI_CALL wrap_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                            VkDeviceSize size, VkMemoryMapFlags flags, void **ppData)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "offset=%llu bytes=%llu", (unsigned long long)offset, (unsigned long long)size);
    TIMED(vkMapMemory, REAL(vkMapMemory)(device, memory, offset, size, flags, ppData), detail);
}

static VkResult VKAPI_CALL wrap_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                                               const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "bytes=%llu usage=%#x",
             pCreateInfo ? (unsigned long long)pCreateInfo->size : 0ull, pCreateInfo ? pCreateInfo->usage : 0u);
    TIMED(vkCreateBuffer, REAL(vkCreateBuffer)(device, pCreateInfo, pAllocator, pBuffer), detail);
}

static VkResult VKAPI_CALL wrap_vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                              const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    if (pCreateInfo)
        snprintf(detail, sizeof(detail), "extent=%ux%ux%u format=%d mips=%u layers=%u usage=%#x",
                 pCreateInfo->extent.width, pCreateInfo->extent.height, pCreateInfo->extent.depth,
                 (int)pCreateInfo->format, pCreateInfo->mipLevels, pCreateInfo->arrayLayers, pCreateInfo->usage);
    else detail[0] = 0;
    TIMED(vkCreateImage, REAL(vkCreateImage)(device, pCreateInfo, pAllocator, pImage), detail);
}

static VkResult VKAPI_CALL wrap_vkAllocateDescriptorSets(VkDevice device,
                                                         const VkDescriptorSetAllocateInfo *pAllocateInfo,
                                                         VkDescriptorSet *pDescriptorSets)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "sets=%u", pAllocateInfo ? pAllocateInfo->descriptorSetCount : 0u);
    TIMED(vkAllocateDescriptorSets, REAL(vkAllocateDescriptorSets)(device, pAllocateInfo, pDescriptorSets), detail);
}

static VkResult VKAPI_CALL wrap_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits,
                                              VkFence fence)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "submits=%u command_buffers=%u fence=%d", submitCount,
             submitCount && pSubmits ? pSubmits[0].commandBufferCount : 0u, fence != VK_NULL_HANDLE);
    TIMED(vkQueueSubmit, REAL(vkQueueSubmit)(queue, submitCount, pSubmits, fence), detail);
}

static VkResult VKAPI_CALL wrap_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits,
                                               VkFence fence)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "submits=%u command_buffers=%u fence=%d", submitCount,
             submitCount && pSubmits ? pSubmits[0].commandBufferInfoCount : 0u, fence != VK_NULL_HANDLE);
    TIMED(vkQueueSubmit2, REAL(vkQueueSubmit2)(queue, submitCount, pSubmits, fence), detail);
}

static VkResult VKAPI_CALL wrap_vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences,
                                                VkBool32 waitAll, uint64_t timeout)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "fences=%u timeout=%llu", fenceCount, (unsigned long long)timeout);
    TIMED(vkWaitForFences, REAL(vkWaitForFences)(device, fenceCount, pFences, waitAll, timeout), detail);
}

static VkResult VKAPI_CALL wrap_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo *pWaitInfo,
                                                 uint64_t timeout)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "semaphores=%u timeout=%llu", pWaitInfo ? pWaitInfo->semaphoreCount : 0u,
             (unsigned long long)timeout);
    TIMED(vkWaitSemaphores, REAL(vkWaitSemaphores)(device, pWaitInfo, timeout), detail);
}

static VkResult VKAPI_CALL wrap_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                      VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "timeout=%llu", (unsigned long long)timeout);
    TIMED(vkAcquireNextImageKHR,
          REAL(vkAcquireNextImageKHR)(device, swapchain, timeout, semaphore, fence, pImageIndex), detail);
}

static VkResult VKAPI_CALL wrap_vkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId,
                                                    uint64_t timeout)
{
    char detail[PW_VK_RADV_PROFILE_DETAIL];
    snprintf(detail, sizeof(detail), "present_id=%llu timeout=%llu", (unsigned long long)presentId,
             (unsigned long long)timeout);
    TIMED(vkWaitForPresentKHR, REAL(vkWaitForPresentKHR)(device, swapchain, presentId, timeout), detail);
}

/* A present ends a frame: the count moves after the call returns, this
 * thread's table rolls over to it at once, and the present line carries the
 * interval since the previous present's return. */
static VkResult VKAPI_CALL wrap_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    char line[PW_VK_RADV_PROFILE_LINE];
    struct pw_vk_radv_profile_thread *t;
    uint64_t t0 = __rdtsc(), t1, frame, previous;
    VkResult result = REAL(vkQueuePresentKHR)(queue, pPresentInfo);
    t1 = __rdtsc();
    t = thread_table();
    if (t)
    {
        rollover(t, t1);
        pw_vk_radv_profile_add(t, PW_VK_RADV_vkQueuePresentKHR, t1 - t0);
    }
    frame = atomic_fetch_add(&state.presents, 1) + 1;
    previous = atomic_exchange(&state.last_present_tsc, t1);
    emit(line, pw_vk_radv_profile_format_present(line, sizeof(line), t ? t->index : 0, frame, t1,
                                                 previous && t1 > previous ? t1 - previous : 0, t1 - t0, (int)result));
    if (t) rollover(t, t1);
    return result;
}

#define PW_VK_RADV_FN(name, ret, params, args) { #name, (PFN_vkVoidFunction)wrap_##name },
#define PW_VK_RADV_FN_VOID(name, params, args) { #name, (PFN_vkVoidFunction)wrap_##name },
#define PW_VK_RADV_FN_HAND(name, ret, params, args) { #name, (PFN_vkVoidFunction)wrap_##name },
static const struct { const char *name; PFN_vkVoidFunction wrapper; } wrappers[PW_VK_RADV_FUNCTION_COUNT] = {
#include "pw_vk_radv_profile_list.h"
};
#undef PW_VK_RADV_FN
#undef PW_VK_RADV_FN_VOID
#undef PW_VK_RADV_FN_HAND

/* The pointer Wine gets for name: the wrapper when the profile is on and
 * name is timed (the real pointer is kept for it), else real. */
PFN_vkVoidFunction pw_vk_radv_profile_resolve(const char *name, PFN_vkVoidFunction real_function)
{
    unsigned fn;
    pthread_mutex_lock(&state.lock);
    initialize_locked();
    if (!state.enabled || !real_function || !name)
    {
        pthread_mutex_unlock(&state.lock);
        return real_function;
    }
    for (fn = 0; fn < PW_VK_RADV_FUNCTION_COUNT; fn++)
    {
        if (strcmp(wrappers[fn].name, name)) continue;
        real[fn] = real_function;
        pthread_mutex_unlock(&state.lock);
        return wrappers[fn].wrapper;
    }
    pthread_mutex_unlock(&state.lock);
    return real_function;
}
