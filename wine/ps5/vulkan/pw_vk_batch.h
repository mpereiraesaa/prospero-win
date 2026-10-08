/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_BATCH_H
#define PW_VK_BATCH_H
#define PW_VK_BATCH_VERSION 1u
#define PW_VK_BATCH_CAPABILITY 0x50570101u
#define PW_VK_BATCH_NAME "__wine_pw_vk_batch_v1"
#define PW_VK_BATCH_ARENA 65536u
#define PW_VK_BATCH_SCRATCH (4u*1024u*1024u)
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
