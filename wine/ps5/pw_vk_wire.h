/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_WIRE_H
#define PW_VK_WIRE_H
#include <stddef.h>
#include <stdint.h>
/* Explicit offsets, little-endian fields. Dispatch handles are zero-extended
 * 32-bit client object addresses; non-dispatch handles remain full 64-bit. */
enum pw_vk_wire_opcode { PW_VK_DRAW_INDEXED=1, PW_VK_BIND_PIPELINE,
 PW_VK_BIND_INDEX, PW_VK_BIND_DESCRIPTORS, PW_VK_BIND_VERTEX2,
 PW_VK_UPDATE_TEMPLATE };
struct pw_vk_template_entry { uint32_t type,count; uint64_t offset,stride; };
/* Vulkan descriptor types 0..10 only; unsupported types must synchronously
 * drain all streams and fall back before accessing their data. */
int pw_vk_template_extent(const struct pw_vk_template_entry *,size_t,size_t *);
int pw_vk_wire_draw(void *,size_t,uint32_t,uint32_t,uint32_t,uint32_t,int32_t,uint32_t,size_t *);
int pw_vk_wire_pipeline(void *,size_t,uint32_t,uint32_t,uint64_t,size_t *);
int pw_vk_wire_index(void *,size_t,uint32_t,uint64_t,uint64_t,uint64_t,uint32_t,uint32_t,size_t *);
int pw_vk_wire_descriptors(void *,size_t,uint32_t,uint32_t,uint64_t,uint32_t,uint32_t,const uint64_t *,uint32_t,const uint32_t *,size_t *);
int pw_vk_wire_vertex2(void *,size_t,uint32_t,uint32_t,uint32_t,const uint64_t *,const uint64_t *,const uint64_t *,const uint64_t *,size_t *);
int pw_vk_wire_template(void *,size_t,uint32_t,uint64_t,uint64_t,const struct pw_vk_template_entry *,size_t,const void *,size_t *);
/* Preflight every record before replay begins. Returns zero for unknown op,
 * bad size/count/flags or unsafe handle width; no dispatch on failure. */
int pw_vk_wire_validate(uint32_t,const void *,size_t);
uint32_t pw_vk_wire_u32(const void *);
uint64_t pw_vk_wire_u64(const void *);
/* Generator inserts these hooks only in 32-bit PE builds. */
int pw_wine_vk_enqueue(unsigned int,void *);
void pw_wine_vk_before_call(void);
void pw_wine_vk_after_call(void);
#endif
