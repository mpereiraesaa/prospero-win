/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_TEMPLATE_CACHE_H
#define PW_VK_TEMPLATE_CACHE_H
#include <stddef.h>
#include <stdint.h>
#include "pw_vk_wire.h"
struct pw_vk_template_meta;
/* Fixed bucket storage avoids allocation or rehash on the hot lookup path.
 * Collisions own separate chains; template capacity is not the bucket count. */
#define PW_VK_TEMPLATE_BUCKETS 1024u
struct pw_vk_template_cache {
 struct pw_vk_template_meta *buckets[PW_VK_TEMPLATE_BUCKETS];
 size_t count;
};
/* All operations require the same process-wide caller lock. Keep that lock
 * across lookup and snapshot encoding; never retain borrowed metadata after
 * unlocking. Allocation is injected so Wine can use HeapAlloc/HeapFree. */
struct pw_vk_template_alloc { void *(*alloc)(size_t); void (*free)(void *); };
/* Register only AFTER VK_SUCCESS, for core and KHR create alike. Unsupported
 * pNext, flags, type, overflow or OOM leaves template unbatchable. Template
 * identity is scoped to client device, and handle remains all 64 bits. */
int pw_vk_template_register(struct pw_vk_template_cache *, const struct pw_vk_template_alloc *,
 uint32_t device,uint64_t handle,int create_success,int pnext_present,uint32_t flags,
 uint32_t template_type,const struct pw_vk_template_entry *,size_t);
const struct pw_vk_template_entry *pw_vk_template_lookup(const struct pw_vk_template_cache *,uint32_t,uint64_t,size_t *);
/* Encode while the caller still holds metadata lock; guest32 enforces the
 * low address ABI bounds when used from real 32-bit Wine thunks. */
int pw_vk_template_snapshot(const struct pw_vk_template_cache *,uint32_t,uint64_t,uint64_t,
 const void *,int guest32,void *,size_t,size_t *);
/* Caller drains every stream BEFORE these operations, without cache lock held,
 * then obtains the lock and retires metadata; then forwards original destroy.
 * Global stream ordering/lifetime synchronization is caller responsibility. */
void pw_vk_template_remove(struct pw_vk_template_cache *,const struct pw_vk_template_alloc *,uint32_t,uint64_t);
void pw_vk_template_remove_device(struct pw_vk_template_cache *,const struct pw_vk_template_alloc *,uint32_t);
#endif
