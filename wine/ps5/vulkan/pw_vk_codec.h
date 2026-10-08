/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_CODEC_H
#define PW_VK_CODEC_H
#include <stddef.h>
#include <stdint.h>
/* Shared fieldwise codec: PE encoding never transfers native addresses. Unix
 * decoding places all arrays/strings/chains in a caller-owned aligned arena. */
#define PW_VK_CODEC_MAX_BYTES (256u*1024u)
#define PW_VK_CODEC_DECODE_BYTES (4u*1024u*1024u)
struct pw_vk_codec {
 unsigned char *wire, *arena;
 size_t capacity, used, arena_capacity, arena_used;
 unsigned decode, depth, gpu_addresses;
};
int pw_vk_codec_source(struct pw_vk_codec *,const void *,size_t);
int pw_vk_codec_value(struct pw_vk_codec *,void *,size_t,unsigned);
int pw_vk_codec_array(struct pw_vk_codec *,void *,uint64_t,size_t,unsigned);
int pw_vk_codec_string(struct pw_vk_codec *,void *);
int pw_vk_generated_encode(unsigned,void *,void *,size_t,size_t *);
int pw_vk_generated_decode(unsigned,const void *,size_t,void *,size_t,void **);
size_t pw_vk_generated_param_size(unsigned);
#endif
