/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_RETIRE_H
#define PW_VK_RETIRE_H
#include <stddef.h>
/* Caller holds the replay ordering gate. Nodes and client objects use separate
 * allocators: client objects must return to Wine's original CRT allocator. */
struct pw_vk_retired { struct pw_vk_retired *next; void *object; };
struct pw_vk_retirement { struct pw_vk_retired *head; };
int pw_vk_retirement_add(struct pw_vk_retirement *,void *,void *(*)(size_t));
void pw_vk_retirement_drain(struct pw_vk_retirement *,void (*)(void *),void (*)(void *));
#endif
