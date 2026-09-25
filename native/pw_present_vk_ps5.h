/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_PRESENT_VK_PS5_H
#define PW_PRESENT_VK_PS5_H
#include "../src/pw_present.h"
#include <ps5vk/ps5vk.h>

/* PwPresentSink over ps5-vulkan's display-plane surface and bounded
 * swapchain: two 1920x1080 BGRA8 transfer-destination images, FIFO.  The
 * backend owns VideoOut and AGC through ps5-vulkan; the caller must not open
 * pw_videoout_ps5 in the same process. */
enum { PW_PRESENT_VK_WIDTH=1920,PW_PRESENT_VK_HEIGHT=1080 };
typedef struct PwPresentVkPs5 {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkSurfaceKHR surface;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[2];
    VkCommandPool pool;
    VkCommandBuffer commands;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    uint8_t *mapped;
    VkFence fence;
    uint64_t flips,submits,last_sequence;
    uint32_t last_slot,lent;
    /* The first failing Vulkan call and its result, for telemetry. */
    const char *failed_call;
    int32_t failed_result;
} PwPresentVkPs5;

int pw_present_vk_ps5_open(PwPresentVkPs5 *,PwPresentSink *sink);
int pw_present_vk_ps5_close(PwPresentVkPs5 *);
#endif
