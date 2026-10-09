/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_BATCH_H
#define PW_VK_BATCH_H
#define PW_VK_BATCH_VERSION 2u
#define PW_VK_BATCH_ASYNC_VERSION 3u
#define PW_VK_BATCH_ASYNC_CAPABILITY 0x50570202u
#define PW_VK_BATCH_ASYNC_NAME "__wine_pw_vk_batch_async_v2"
#define PW_VK_BATCH_LEGACY_VERSION 1u
#define PW_VK_BATCH_CAPABILITY 0x50570201u
#define PW_VK_BATCH_NAME "__wine_pw_vk_batch_v2"
#define PW_VK_BATCH_LEGACY_NAME "__wine_pw_vk_batch_v1"
#define PW_VK_BATCH_LEGACY_CAPABILITY 0x50570101u
#define PW_VK_BATCH_GENERATED_OPCODE 8u
#define PW_VK_BATCH_ARENA (256u*1024u)
#define PW_VK_BATCH_SCRATCH (16u*1024u*1024u)
/* All pointer fields are explicit 32-bit PE addresses. Result/status is owned
 * by the ordinary thunk; the batch entry reports only its own framing error. */
struct pw_vk_batch_params { UINT32 version,batch,bytes,code,args,status; };
NTSTATUS pw_vk_batch_unix(void *);
#ifndef _WIN64
BOOL pw_vk_batch_allocator(unsigned int,const void *);
NTSTATUS pw_vk_batch_call(unsigned int,void *);
void pw_vk_batch_thread_detach(void);
void pw_vk_batch_retire_free(void *);
#endif
#endif
