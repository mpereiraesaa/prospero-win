/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* RADV's libvulkan.prx entry points. Wine's win32u looks up
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr in libvulkan.so; Mesa's
 * archive exports only the loader's ICD interface, whose instance lookup
 * resolves every entry point and whose device lookup is the runtime's own.
 * The title's own presenter shows GDI frames through RADV's VideoOut WSI
 * while no swapchain presents (pw_videoout_idle, pw_videoout_show_tiled),
 * and win32u shows the application's cursor over the swapchain's frames
 * with VideoOut's hardware cursor (pw_videoout_cursor).
 * Linked by tools/link_radv_prx.sh. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <vulkan/vulkan.h>
#if defined(__PROSPERO__)
#include <x86intrin.h>
#endif

PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);
bool wsi_videoout_idle(void);
int wsi_videoout_show_tiled(const void *tiled, uint64_t bytes, uint32_t width, uint32_t height);
bool wsi_videoout_present_rect(VkRect2D *rect, VkExtent2D *frame);
int pw_videoout_idle(void);
int pw_videoout_show_tiled(const void *tiled, uint64_t bytes, uint32_t width, uint32_t height);
int pw_videoout_cursor(const uint32_t *argb, uint32_t width, uint32_t height, int32_t x, int32_t y,
                       uint32_t space_width, uint32_t space_height);

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

/* --- the hardware cursor -------------------------------------------------
 * VideoOut composes up to two 64x64 cursor images over whatever the output
 * shows, at a position in the shown frame's pixels (measured on FW 12.02:
 * enable, image and position calls return 0 and the image moves without
 * tearing).
 * RADV's WSI holds the output's handle; tools/link_radv_prx.sh wraps its
 * sceVideoOutOpen so this file learns it. */
extern int __real_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
extern int sceVideoOutCursorEnable(int32_t handle, int32_t index, const void *address);
extern int sceVideoOutCursorDisable(int32_t handle, int32_t index);
extern int sceVideoOutCursorSetImageAddress(int32_t handle, int32_t index, const void *address);
extern int sceVideoOutCursorSetPosition(int32_t handle, int32_t index, uint32_t x, uint32_t y);
extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t bytes, size_t alignment, int type,
                                         int64_t *offset);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset,
                                    size_t alignment);

enum { CURSOR_SIDE = 64, CURSOR_PIXELS = CURSOR_SIDE * CURSOR_SIDE, CURSOR_MEMORY = 64 << 10 };

static int videoout_handle = -1;
static struct {
    int lock;
    /* Two images in display-readable direct memory, written alternately so
     * the one on screen never changes under the display. */
    uint32_t *images;
    int failed, enabled, current;
    uint32_t shown[CURSOR_PIXELS];
} cursor;

int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter);
int __wrap_sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void *parameter)
{
    int handle = __real_sceVideoOutOpen(user, bus, index, parameter);

    if (handle >= 0) __atomic_store_n(&videoout_handle, handle, __ATOMIC_RELEASE);
    return handle;
}

static void cursor_lock(void)
{
    while (__atomic_exchange_n(&cursor.lock, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
}

static void cursor_unlock(void)
{
    __atomic_store_n(&cursor.lock, 0, __ATOMIC_RELEASE);
}

/* Under the lock: the images' memory, once. */
static int cursor_setup(void)
{
    int64_t offset;
    void *address = NULL;

    if (cursor.images) return 0;
    if (cursor.failed) return -1;
    if (sceKernelAllocateDirectMemory(0, (int64_t)sceKernelGetDirectMemorySize(), CURSOR_MEMORY, CURSOR_MEMORY, 3,
                                      &offset) < 0 ||
        sceKernelMapDirectMemory(&address, CURSOR_MEMORY, 0x33, 0, offset, CURSOR_MEMORY) < 0) {
        cursor.failed = 1;
        fprintf(stderr, "pw_videoout_cursor: no display memory for the cursor\n");
        return -1;
    }
    cursor.images = address;
    return 0;
}

static void cursor_hide_locked(int handle)
{
    if (cursor.enabled) sceVideoOutCursorDisable(handle, 0);
    cursor.enabled = 0;
}

/* Shows the application's cursor over a swapchain's frames: argb is its
 * width x height image in straight-alpha ARGB (B8G8R8A8 bytes), cut to 64x64,
 * with its top-left corner at (x, y) of a space_width x space_height desktop,
 * which the swapchain's frames show whole. NULL hides it. 0 shown or hidden, 1 no
 * output yet, <0 failed. */
int pw_videoout_cursor(const uint32_t *argb, uint32_t width, uint32_t height, int32_t x, int32_t y,
                       uint32_t space_width, uint32_t space_height)
{
    int handle = __atomic_load_n(&videoout_handle, __ATOMIC_ACQUIRE), rc = 0;
    uint32_t image[CURSOR_PIXELS];

    if (handle < 0) return 1;
    if (!argb || !width || !height || !space_width || !space_height) {
        cursor_lock();
        cursor_hide_locked(handle);
        cursor_unlock();
        return 0;
    }
    memset(image, 0, sizeof(image));
    for (uint32_t row = 0; row < height && row < CURSOR_SIDE; row++)
        memcpy(image + row * CURSOR_SIDE, argb + (size_t)row * width,
               (width < CURSOR_SIDE ? width : CURSOR_SIDE) * sizeof(uint32_t));
    cursor_lock();
    if (cursor_setup()) {
        cursor_unlock();
        return -1;
    }
    if (!cursor.enabled || memcmp(image, cursor.shown, sizeof(image))) {
        uint32_t *next = cursor.images + (cursor.enabled ? (cursor.current ^ 1) : cursor.current) * CURSOR_PIXELS;

        memcpy(next, image, sizeof(image));
        memcpy(cursor.shown, image, sizeof(image));
#if defined(__PROSPERO__)
        /* The display reads memory, not the CPU's cache. */
        for (size_t at = 0; at < sizeof(image); at += 64) _mm_clflush((const uint8_t *)next + at);
        _mm_mfence();
#endif
        rc = cursor.enabled ? sceVideoOutCursorSetImageAddress(handle, 0, next)
                            : sceVideoOutCursorEnable(handle, 0, next);
        if (rc == 0) {
            cursor.current = (int)((next - cursor.images) / CURSOR_PIXELS);
            cursor.enabled = 1;
        }
    }
    /* The position is in the shown frame's pixels, not the output's: at a
     * 3840x2160 output, (928, 508) is the middle of a 1920x1080 frame
     * (measured), so a desktop the frame shows whole needs no scaling. A
     * swapchain of a size VideoOut does not take shows scaled and centred
     * in a framebuffer of one it does (RADV's WSI), where win32u maps the
     * display mode onto the desktop from its top left, scaled alike: the
     * position moves by the image's offset there, scaled by the framebuffer
     * to the desktop. */
    if (cursor.enabled) {
        VkRect2D rect;
        VkExtent2D frame;
        int64_t fx = x > 0 ? x : 0, fy = y > 0 ? y : 0;

        if (wsi_videoout_present_rect(&rect, &frame) && frame.width && frame.height) {
            fx = rect.offset.x + fx * frame.width / space_width;
            fy = rect.offset.y + fy * frame.height / space_height;
        }
        rc = sceVideoOutCursorSetPosition(handle, 0, (uint32_t)fx, (uint32_t)fy);
    }
    cursor_unlock();
    return rc == 0 ? 0 : -2;
}

/* Shows a B8G8R8A8 frame already in VideoOut's tiling while no swapchain
 * presents: 0 shown, 1 a swapchain is presenting, <0 failed. A GDI frame
 * has the cursor drawn in, so the hardware cursor is hidden while one shows;
 * the next swapchain present brings it back. */
int pw_videoout_show_tiled(const void *tiled, uint64_t bytes, uint32_t width, uint32_t height)
{
    int result = wsi_videoout_show_tiled(tiled, bytes, width, height);
    int handle = __atomic_load_n(&videoout_handle, __ATOMIC_ACQUIRE);

    if (result == 0 && handle >= 0) {
        cursor_lock();
        cursor_hide_locked(handle);
        cursor_unlock();
    }
    return result;
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
