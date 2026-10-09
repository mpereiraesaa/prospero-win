/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_ASYNC_H
#define PW_VK_ASYNC_H
/* Called on ordinary Wine API threads before native lifecycle/submit work. */
unsigned pw_vk_async_pool_fanout_count(void);
void pw_vk_async_wait_buffer(VkCommandBuffer);
void pw_vk_async_wait_buffer_pool(VkCommandBuffer);
void pw_vk_async_wait_pool(VkCommandPool);
void pw_vk_async_forget_buffer(VkCommandBuffer);
#endif
