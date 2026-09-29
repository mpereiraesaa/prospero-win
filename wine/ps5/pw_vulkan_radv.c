/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* RADV's libvulkan.prx entry points. Wine's win32u looks up
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr in libvulkan.so; Mesa's
 * archive exports only the loader's ICD interface, whose instance lookup
 * resolves every entry point and whose device lookup is the runtime's own.
 * Linked by tools/link_radv_prx.sh. */
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return vk_icdGetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    return vk_common_GetDeviceProcAddr(device, name);
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
