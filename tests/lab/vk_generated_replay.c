/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Actual Unix entry/codec/core with controlled native thunks; no GPU driver. */
#include "config.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include "vulkan_private.h"
#include "pw_vk_batch.h"
#include "pw_vk_codec.h"
#include "pw_vk_wire.h"
#include "pw_vk_command_stream.h"
static _Thread_local uint64_t scalar_calls;
int __real_pw_vk_codec_value(struct pw_vk_codec *,void *,size_t,unsigned);
int __wrap_pw_vk_codec_value(struct pw_vk_codec *c,void *p,size_t n,unsigned wide)
{ scalar_calls++;return __real_pw_vk_codec_value(c,p,n,wide); }
static _Thread_local unsigned effects;
static _Thread_local int fail_allocation;
static atomic_uint allocations,releases;
void *__real_malloc(size_t);
void __real_free(void *);
void *__wrap_malloc(size_t bytes)
{ assert(bytes==PW_VK_CODEC_DECODE_BYTES);atomic_fetch_add(&allocations,1);return fail_allocation?NULL:__real_malloc(bytes); }
void __wrap_free(void *p)
{ if(p)atomic_fetch_add(&releases,1);__real_free(p); }
static _Thread_local struct pw_vk_batch_params *nested;
static _Thread_local int inside_nested;
static _Thread_local unsigned expected_count;
static NTSTATUS call_batch(struct pw_vk_batch_params *p)
{ return pw_vk_batch_unix(p); }

NTSTATUS pw_vk_batch_dispatch_native(unsigned code,void *params)
{
 if(code==unix_vkCmdPipelineBarrier2){
  struct vkCmdPipelineBarrier2_params *p=params;
  assert(p->commandBuffer==(VkCommandBuffer)(uintptr_t)0x1234);
  unsigned saved_count=p->pDependencyInfo->memoryBarrierCount;
  assert(saved_count>=1&&(!expected_count||saved_count==expected_count));
  assert((uintptr_t)p->pDependencyInfo%8==0);
  assert(p->pDependencyInfo->pMemoryBarriers[p->pDependencyInfo->memoryBarrierCount-1].srcStageMask==0x12345678);
  if(nested&&!inside_nested){inside_nested=1;assert(call_batch(nested)==STATUS_SUCCESS);inside_nested=0;assert(p->pDependencyInfo->memoryBarrierCount==saved_count);}
  assert(p->pDependencyInfo->pMemoryBarriers[0].srcStageMask==0x12345678);
 }else if(code==unix_vkCmdSetViewport){
  struct vkCmdSetViewport_params *p=params;assert(p->viewportCount==1&&p->pViewports[0].width==640&&p->pViewports[0].height==480);
 }else if(code==unix_vkUpdateDescriptorSets){
  struct vkUpdateDescriptorSets_params *p=params;assert(p->descriptorWriteCount==1&&p->pDescriptorWrites[0].pBufferInfo->buffer==0x4433);
 }else if(code==unix_vkDestroyDevice){
  struct vkDestroyDevice_params *p=params;
  assert(p->device==(VkDevice)(uintptr_t)0x5678&&!p->pAllocator);
 }else if(code==unix_vkCmdUpdateBuffer){
  struct vkCmdUpdateBuffer_params *p=params;
  assert(p->dataSize==65536&&((const unsigned char *)p->pData)[0]==0x5a&&((const unsigned char *)p->pData)[65535]==0x5a);
 }else abort();
 effects++;return STATUS_SUCCESS;
}
NTSTATUS pw_vk_batch_dispatch(unsigned code,void *params)
{ assert(code==unix_vkDeviceWaitIdle&&params==(void *)(uintptr_t)0x3456&&effects==2);effects++;return STATUS_TIMEOUT; }
static void append(struct pw_vk_stream_registry *registry,struct pw_vk_stream *stream,unsigned code,void *params)
{
 unsigned char encoded[65536];size_t bytes;
 assert(pw_vk_generated_encode(code,params,encoded+4,sizeof(encoded)-4,&bytes));
 memcpy(encoded,&code,4);
 assert(pw_vk_stream_append(registry,stream,PW_VK_BATCH_GENERATED_OPCODE,encoded,bytes+4)==PW_VK_STREAM_OK);
 memset(encoded,0xcc,sizeof(encoded));
}
struct thread_job { struct pw_vk_batch_params call; unsigned count; };
static void *concurrent(void *argument)
{
 struct thread_job *job=argument;struct pw_vk_batch_params p=job->call;expected_count=job->count;
 for(unsigned i=0;i<100;i++)assert(call_batch(&p)==STATUS_SUCCESS);
 return NULL;
}
static double seconds(void)
{ struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+t.tv_nsec*1e-9; }
static void legacy_draw(VkCommandBuffer command,uint32_t count,uint32_t instances,uint32_t first,int32_t vertex,uint32_t instance)
{ (void)command;(void)instances;(void)first;(void)vertex;(void)instance;assert(count==1);effects++; }
static int legacy_benchmark(void)
{
 void *batch=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(batch!=MAP_FAILED);
 struct vulkan_device device={0};struct vulkan_command_buffer buffer={0};struct vulkan_client_object *client=(void *)((unsigned char *)batch+64000);
 device.p_vkCmdDrawIndexed=legacy_draw;buffer.device=&device;client->unix_handle=(uintptr_t)&buffer;
 struct pw_vk_stream_registry registry;struct pw_vk_stream stream={0};unsigned char storage[8192],wire[24];size_t n,bytes,records;
 pw_vk_stream_registry_init(&registry);assert(!pw_vk_stream_register(&registry,&stream,storage,sizeof(storage)));
 assert(pw_vk_wire_draw(wire,sizeof(wire),(uintptr_t)client,1,1,0,0,0,&n));
 for(unsigned i=0;i<24;i++)assert(!pw_vk_stream_append(&registry,&stream,PW_VK_DRAW_INDEXED,wire,n));
 assert(!pw_vk_stream_collect(&registry,batch,64000,&bytes,&records));
 struct pw_vk_batch_params p={.version=PW_VK_BATCH_LEGACY_VERSION,.batch=(uintptr_t)batch,.bytes=bytes,.code=unix_count};
 const unsigned iterations=100000;double start=seconds();unsigned before=atomic_load(&allocations);
 for(unsigned i=0;i<iterations;i++)assert(!call_batch(&p));
 printf("legacy_batches=%u records_per_batch=24 elapsed_seconds=%.6f allocations=%u\n",iterations,seconds()-start,atomic_load(&allocations)-before);
 assert(!pw_vk_stream_unregister(&registry,&stream));assert(!munmap(batch,65536));return 0;
}
/* Encode, frame, semantic preflight and native replay in each iteration. */
static int blob_benchmark(void)
{
 unsigned char input[65536],encoded[131072],storage[131072];size_t bytes,n,records;
 memset(input,0x5a,sizeof(input));
 struct vkCmdUpdateBuffer_params command={.commandBuffer=(VkCommandBuffer)(uintptr_t)0x1234,.dstBuffer=0x4433,.dataSize=sizeof(input),.pData=input};
 void *batch=mmap(NULL,sizeof(storage),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(batch!=MAP_FAILED);
 struct pw_vk_stream_registry registry;struct pw_vk_stream stream={0};
 pw_vk_stream_registry_init(&registry);assert(!pw_vk_stream_register(&registry,&stream,storage,sizeof(storage)));
 struct pw_vk_batch_params call={.version=PW_VK_BATCH_VERSION,.batch=(uintptr_t)batch,.code=unix_count};
 const unsigned iterations=1000;scalar_calls=0;unsigned before=atomic_load(&allocations);double start=seconds();
 for(unsigned i=0;i<iterations;i++){
  assert(pw_vk_generated_encode(unix_vkCmdUpdateBuffer,&command,encoded+4,sizeof(encoded)-4,&n));
  unsigned code=unix_vkCmdUpdateBuffer;memcpy(encoded,&code,4);
  assert(!pw_vk_stream_append(&registry,&stream,PW_VK_BATCH_GENERATED_OPCODE,encoded,n+4));
  assert(!pw_vk_stream_collect(&registry,batch,sizeof(storage),&bytes,&records)&&records==1);call.bytes=bytes;
  assert(!call_batch(&call));
 }
 printf("blob_bytes=65536 full_path_batches=%u elapsed_seconds=%.6f allocations=%u generated_scalar_calls=%llu\n",iterations,seconds()-start,atomic_load(&allocations)-before,(unsigned long long)scalar_calls);
 assert(!pw_vk_stream_unregister(&registry,&stream));assert(!munmap(batch,sizeof(storage)));return 0;
}
int main(int argc,char **argv)
{
 if(argc==2&&!strcmp(argv[1],"blob-benchmark"))return blob_benchmark();
 if(argc==2&&!strcmp(argv[1],"legacy-benchmark"))return legacy_benchmark();
 struct pw_vk_stream_registry registry;struct pw_vk_stream stream={0};
 unsigned char arena[65536];size_t bytes,records;struct pw_vk_batch_params call;
 VkMemoryBarrier2 barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=0x12345678};
 VkDependencyInfo info={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&barrier};
 struct vkCmdPipelineBarrier2_params command={.commandBuffer=(VkCommandBuffer)(uintptr_t)0x1234,.pDependencyInfo=&info};
 struct vkDestroyDevice_params destroy={.device=(VkDevice)(uintptr_t)0x5678};
 void *batch=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(batch!=MAP_FAILED&&(uintptr_t)batch<=UINT32_MAX);
 pw_vk_stream_registry_init(&registry);assert(!pw_vk_stream_register(&registry,&stream,arena,sizeof(arena)));
 append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);append(&registry,&stream,unix_vkDestroyDevice,&destroy);
 memset(&barrier,0xcc,sizeof(barrier));memset(&info,0xcc,sizeof(info));
 assert(!pw_vk_stream_collect(&registry,batch,65536,&bytes,&records)&&records==2);
 call=(struct pw_vk_batch_params){.version=PW_VK_BATCH_VERSION,.batch=(uintptr_t)batch,.bytes=bytes,.code=unix_vkDeviceWaitIdle,.args=0x3456};
 assert(pw_vk_batch_unix(&call)==STATUS_SUCCESS&&call.status==STATUS_TIMEOUT&&effects==3);
#ifndef PW_VK_ORIGINAL_ARENA
 assert(atomic_load(&allocations)==0&&atomic_load(&releases)==0);
#endif
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
 /* Rebuild a valid small batch for recursive and parallel native entries.
  * Every invocation must retain its own decoded parameter/array storage. */
 call.code=unix_count;effects=0;
 barrier=(VkMemoryBarrier2){.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=0x12345678};
 info=(VkDependencyInfo){.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&barrier};
 append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);
 assert(!pw_vk_stream_collect(&registry,batch,65536,&bytes,&records));call.bytes=bytes;
 VkMemoryBarrier2 independent[4];for(unsigned i=0;i<4;i++)independent[i]=barrier;
 info.pMemoryBarriers=independent;info.memoryBarrierCount=2;
 append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);
 void *inner=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(inner!=MAP_FAILED);
 size_t inner_bytes;assert(!pw_vk_stream_collect(&registry,inner,65536,&inner_bytes,&records));
 struct pw_vk_batch_params inner_call=call;inner_call.batch=(uintptr_t)inner;inner_call.bytes=inner_bytes;
 nested=&inner_call;assert(!call_batch(&call)&&effects==2);nested=NULL;assert(!munmap(inner,65536));
 pthread_t threads[4];struct thread_job jobs[4];void *thread_batches[4];
 for(unsigned i=0;i<4;i++){
  info.memoryBarrierCount=i+1;append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);
  thread_batches[i]=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);assert(thread_batches[i]!=MAP_FAILED);
  assert(!pw_vk_stream_collect(&registry,thread_batches[i],65536,&inner_bytes,&records));
  jobs[i].call=call;jobs[i].call.batch=(uintptr_t)thread_batches[i];jobs[i].call.bytes=inner_bytes;jobs[i].count=i+1;
 }
 for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,concurrent,&jobs[i]));
 for(unsigned i=0;i<4;i++){assert(!pthread_join(threads[i],NULL));assert(!munmap(thread_batches[i],65536));}
 info.memoryBarrierCount=1;info.pMemoryBarriers=&barrier;
#ifndef PW_VK_ORIGINAL_ARENA
 assert(atomic_load(&allocations)==0&&atomic_load(&releases)==0);
#endif
 if(argc==2&&!strcmp(argv[1],"benchmark")){
  VkViewport viewport={.width=640,.height=480};
  struct vkCmdSetViewport_params vp={.commandBuffer=(VkCommandBuffer)(uintptr_t)0x1234,.viewportCount=1,.pViewports=&viewport};
  VkDescriptorBufferInfo buffer_info={.buffer=0x4433,.offset=16,.range=128};
  VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer_info};
  struct vkUpdateDescriptorSets_params update={.device=(VkDevice)(uintptr_t)0x5678,.descriptorWriteCount=1,.pDescriptorWrites=&write};
  for(unsigned i=0;i<8;i++){
   append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);
   append(&registry,&stream,unix_vkCmdSetViewport,&vp);
   append(&registry,&stream,unix_vkUpdateDescriptorSets,&update);
  }
  assert(!pw_vk_stream_collect(&registry,batch,65536,&bytes,&records));call.bytes=bytes;
  const unsigned iterations=100000;unsigned before=atomic_load(&allocations);double start=seconds();
  for(unsigned i=0;i<iterations;i++)assert(!call_batch(&call));
  printf("small_batches=%u records_per_batch=24 elapsed_seconds=%.6f allocations=%u\n",iterations,seconds()-start,atomic_load(&allocations)-before);
 }
 /* Large valid nested arrays grow only once, including both semantic passes.
  * Allocation failure still rejects without driver effects. */
 VkMemoryBarrier2 large[256];for(unsigned i=0;i<256;i++)large[i]=barrier;
 info.memoryBarrierCount=256;info.pMemoryBarriers=large;
 append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);
 append(&registry,&stream,unix_vkCmdPipelineBarrier2,&command);
 memset(large,0xcc,sizeof(large));
 assert(!pw_vk_stream_collect(&registry,batch,65536,&bytes,&records));call.bytes=bytes;
 effects=0;unsigned before=atomic_load(&allocations),freed=atomic_load(&releases);
 assert(!call_batch(&call)&&effects==2);
 assert(atomic_load(&allocations)==before+1&&atomic_load(&releases)==freed+1);
 fail_allocation=1;effects=0;
 assert(call_batch(&call)==STATUS_NO_MEMORY&&!effects);fail_allocation=0;
 /* Exact arena boundaries are checked independently of schema expansion. */
 void *bounded=mmap(NULL,PW_VK_CODEC_DECODE_BYTES,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bounded!=MAP_FAILED);
 for(size_t limit=4096;limit<=PW_VK_CODEC_DECODE_BYTES;limit=PW_VK_CODEC_DECODE_BYTES){
  uint32_t present=1;void *pointer=NULL;struct pw_vk_codec c={.wire=(void *)&present,.capacity=4,.arena=bounded,.arena_capacity=limit,.decode=1};
  assert(pw_vk_codec_array(&c,&pointer,limit/8,8,0)==1&&c.arena_used==limit);
  c.used=c.arena_used=0;assert(pw_vk_codec_array(&c,&pointer,limit/8+1,8,0)<0);
  if(limit==PW_VK_CODEC_DECODE_BYTES)break;
 }
 assert(!munmap(bounded,PW_VK_CODEC_DECODE_BYTES));
 puts("PASS independent aligned arenas, small zero-allocation replay, one bounded large growth, recursion/concurrency and allocation failure");
 assert(!pw_vk_stream_unregister(&registry,&stream));munmap(batch,65536);
 return 0;
}
