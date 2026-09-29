/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* RADV's libvulkan.prx entry points. Wine's win32u looks up
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr in libvulkan.so; Mesa's
 * archive exports only the loader's ICD interface, whose instance lookup
 * resolves every entry point and whose device lookup is the runtime's own.
 * The title's own presenter shows GDI frames through RADV's VideoOut WSI
 * while no swapchain presents (pw_videoout_idle, pw_videoout_show_tiled).
 * Linked by tools/link_radv_prx.sh. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);
bool wsi_videoout_idle(void);
int wsi_videoout_show_tiled(const void *tiled, uint64_t bytes, uint32_t width, uint32_t height);
int pw_videoout_idle(void);
int pw_videoout_show_tiled(const void *tiled, uint64_t bytes, uint32_t width, uint32_t height);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    return vk_common_GetDeviceProcAddr(device, name);
}

/* 1 when a frame given to pw_videoout_show_tiled would show: the video
 * output is RADV's and no swapchain is presenting. */
int pw_videoout_idle(void)
{
    return wsi_videoout_idle();
}

/* Shows a B8G8R8A8 frame already in VideoOut's tiling while no swapchain
 * presents: 0 shown, 1 a swapchain is presenting, <0 failed. */
int pw_videoout_show_tiled(const void *tiled, uint64_t bytes, uint32_t width, uint32_t height)
{
    return wsi_videoout_show_tiled(tiled, bytes, width, height);
}

/* Mesa's GPU description reads /proc/self/fd links, which a title does not
 * have; the libc readlink is only in libkernel_sys, which a title does not
 * get. Answer as a missing link would. */
ssize_t readlink(const char *path, char *buffer, size_t capacity)
{
    (void)path;
    (void)buffer;
    (void)capacity;
    errno = ENOENT;
    return -1;
}
