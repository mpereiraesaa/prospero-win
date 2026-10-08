/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual Unix entry/codec/core with controlled native thunks; no GPU driver. */
#include "config.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "vulkan_private.h"
#include "pw_vk_batch.h"
#include "pw_vk_codec.h"
#include "pw_vk_command_stream.h"
static unsigned effects;
NTSTATUS pw_vk_batch_dispatch_native(unsigned code,void *params)
{
 if(code==unix_vkCmdPipelineBarrier2){
  struct vkCmdPipelineBarrier2_params *p=params;
  assert(effects==0&&p->commandBuffer==(VkCommandBuffer)(uintptr_t)0x1234);
  assert(p->pDependencyInfo->memoryBarrierCount==1);
  assert(p->pDependencyInfo->pMemoryBarriers[0].srcStageMask==0x12345678);
 }else if(code==unix_vkDestroyDevice){
  struct vkDestroyDevice_params *p=params;
  assert(effects==1&&p->device==(VkDevice)(uintptr_t)0x5678&&!p->pAllocator);
 }else abort();
 effects++;return STATUS_SUCCESS;
}
NTSTATUS pw_vk_batch_dispatch(unsigned code,void *params)
{ assert(code==unix_vkDeviceWaitIdle&&params==(void *)(uintptr_t)0x3456&&effects==2);effects++;return STATUS_TIMEOUT; }
static void append(struct pw_vk_stream_registry *registry,struct pw_vk_stream *stream,unsigned code,void *params)
{
 unsigned char encoded[4096];size_t bytes;
 assert(pw_vk_generated_encode(code,params,encoded+4,sizeof(encoded)-4,&bytes));
 memcpy(encoded,&code,4);
 assert(pw_vk_stream_append(registry,stream,PW_VK_BATCH_GENERATED_OPCODE,encoded,bytes+4)==PW_VK_STREAM_OK);
 memset(encoded,0xcc,sizeof(encoded));
}
int main(void)
{
 struct pw_vk_stream_registry registry;struct pw_vk_stream stream={0};
 unsigned char arena[8192];size_t bytes,records;struct pw_vk_batch_params call;
 VkMemoryBarrier2 barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=0x12345678};
 VkDependencyInfo info={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&barrier};
 struct vkCmdPipelineBarrier2_params command={.commandBuffer=(VkCommandBuffer)(uintptr_t)0x1234,.pDependencyInfo=&info};
 struct vkDestroyDevice_params destroy={.device=(VkDevice)(uintptr_t)0x5678};
 void *batch=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(batch!=MAP_FAILED&&(uintptr_t)batch<=UINT32_MAX);
 pw_vk_stream_registry_init(&registry);assert(!pw_vk_stream_register(&registry,&stream,arena,sizeof(arena)));
 append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);append(&registry,&stream,unix_vkDestroyDevice,&destroy);
 memset(&barrier,0xcc,sizeof(barrier));memset(&info,0xcc,sizeof(info));
 assert(!pw_vk_stream_collect(&registry,batch,8192,&bytes,&records)&&records==2);
 call=(struct pw_vk_batch_params){.version=PW_VK_BATCH_VERSION,.batch=(uintptr_t)batch,.bytes=bytes,.code=unix_vkDeviceWaitIdle,.args=0x3456};
 assert(pw_vk_batch_unix(&call)==STATUS_SUCCESS&&call.status==STATUS_TIMEOUT&&effects==3);
 /* Generic records cannot enter through the legacy protocol. */
 effects=0;call.version=PW_VK_BATCH_LEGACY_VERSION;
 assert(pw_vk_batch_unix(&call)==STATUS_INVALID_PARAMETER&&!effects);
 /* A bad later record must reject the entire batch before any driver effect. */
 call.version=PW_VK_BATCH_VERSION;call.code=unix_count;
 unsigned char *second=(unsigned char *)batch+PW_VK_STREAM_HEADER;
 unsigned first_size;memcpy(&first_size,(unsigned char *)batch+8,4);
 /* Use framing's payload length and aligned record span, not struct packing. */
 second=(unsigned char *)batch+first_size;
 unsigned invalid=unix_count;memcpy(second+PW_VK_STREAM_HEADER,&invalid,4);
 assert(pw_vk_batch_unix(&call)==STATUS_INVALID_PARAMETER&&!effects);
 call.code=unix_pw_vk_batch;assert(pw_vk_batch_unix(&call)==STATUS_INVALID_PARAMETER);
 assert(!pw_vk_stream_unregister(&registry,&stream));munmap(batch,8192);
 return 0;
}
