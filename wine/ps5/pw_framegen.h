/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_FRAMEGEN_H
#define PW_FRAMEGEN_H
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Frame generation's lookup step (pw_framegen.cpp): with PW_FRAMEGEN set
 * to 2..4, the wrapper for an entry point it serves (device creation, queue
 * lookups, swapchain creation and destruction, present), remembering the
 * driver's own; otherwise, or for any other entry point, real itself.
 * instance is the instance the lookup came through, or VK_NULL_HANDLE. */
PFN_vkVoidFunction pw_framegen_resolve(VkInstance instance, const char *name, PFN_vkVoidFunction real);
#ifdef __cplusplus
}
#endif
#endif
