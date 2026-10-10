/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_RADV_PROFILE_WRAP_H
#define PW_VK_RADV_PROFILE_WRAP_H
#include <vulkan/vulkan.h>

/* The profile's side of libvulkan.prx's loader shim (pw_vk_radv_profile.h).
 * resolve returns what Wine gets for an entry point: a timing wrapper while
 * the profile is on (PW_VK_RADV_PROFILE=1 or PW_NATIVE_PROFILE=1 in the
 * environment, or a frame hook installed), else the driver's pointer. The
 * frame hook is called by each thread that enters RADV after a present,
 * with the present count and the TSC; wow64native installs its per-frame
 * report there. Installing a hook turns the profile on. */
typedef void (*pw_vk_radv_frame_hook)(unsigned long long frame, unsigned long long tsc);
PFN_vkVoidFunction pw_vk_radv_profile_resolve(const char *name, PFN_vkVoidFunction real);
void pw_vk_radv_profile_set_frame_hook(pw_vk_radv_frame_hook hook);

#endif
