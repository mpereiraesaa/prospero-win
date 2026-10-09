/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vulkan_loader.h"
#include "pw_vk_batch.h"
#include <stdlib.h>
#include "pw_vk_retire.h"
#include "pw_vk_codec.h"
#include "pw_vk_command_stream.h"
#include "pw_vk_spsc.h"
#include "pw_vk_template_cache.h"
#ifndef _WIN64
#include "pw_vk_disable_guard.h"
#include "pw_vk_function_names.h"
#include "pw_vk_progress_guard.h"
WINE_DEFAULT_DEBUG_CHANNEL(vulkan);
struct producer {
 struct producer *next;struct pw_vk_spsc stream;LONG retired,publishing;
 unsigned char arena[PW_VK_BATCH_ARENA],wire[PW_VK_BATCH_ARENA];
};
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION gate,registry_gate,metadata_gate;
static DWORD tls=TLS_OUT_OF_INDEXES;
static struct { struct producer *streams; } registry;
static struct pw_vk_spsc_sequence stream_sequence;
static uint64_t next_replay=1;
static LONG quiescing;
static struct pw_vk_template_cache templates;
static struct pw_vk_retirement retirement;
static unsigned char *scratch;
static BOOL enabled,stats_enabled;
static _Atomic int negotiated;
/* Only the existing gated fallback path owns these arrays. */
static BOOL fallback_profile;
static UINT64 fallback_counts[unix_count],fallback_reported[unix_count];
static UINT64 fallback_report_present;
/* Opcode bit N-1 matches stable wire opcode N. Not part of the wire ABI. */
static uint32_t opcode_mask=0x7f;
static LONG sticky_disabled;
static DWORD owner;
static unsigned depth;
static UINT64 records_total,dispatches_total,piggyback_total,full_total,fallback_total,enqueued_total;
static DECLSPEC_ALIGN(8) UINT64 present;
static DECLSPEC_ALIGN(8) UINT64 crossings_total;
static void *heap_alloc(size_t n){return HeapAlloc(GetProcessHeap(),0,n);}
static void heap_free(void *p){HeapFree(GetProcessHeap(),0,p);}
static const struct pw_vk_template_alloc allocator={heap_alloc,heap_free};
static DECLSPEC_NORETURN void fatal(void){ERR("PW_VK_BATCH fatal replay/order failure\n");TerminateProcess(GetCurrentProcess(),3);ExitProcess(3);}
static BOOL read_opcode_mask(void)
{
 char value[32];DWORD n=GetEnvironmentVariableA("PW_VK_BATCH_MASK",value,sizeof(value));
 unsigned base=10,i=0,digit;uint32_t mask=0;
 if(!n)return TRUE; /* Unset retains all seven existing categories. */
 if(n>=sizeof(value))return FALSE;
 if(n>=2&&value[0]=='0'&&(value[1]=='x'||value[1]=='X')){base=16;i=2;}
 if(i==n)return FALSE;
 for(;i<n;i++){
  if(value[i]>='0'&&value[i]<='9')digit=value[i]-'0';
  else if(base==16&&value[i]>='a'&&value[i]<='f')digit=value[i]-'a'+10;
  else if(base==16&&value[i]>='A'&&value[i]<='F')digit=value[i]-'A'+10;
  else return FALSE;
  if(digit>=base||mask>(0x7f-digit)/base)return FALSE;
  mask=mask*base+digit;
 }
 opcode_mask=mask;return TRUE;
}
static BOOL CALLBACK initialize(INIT_ONCE *o,void *p,void **ctx)
{
 char env[8];(void)o;(void)p;(void)ctx;
 stats_enabled=GetEnvironmentVariableA("PW_VK_BATCH_STATS",env,sizeof(env))==1&&env[0]=='1';
 fallback_profile=stats_enabled&&GetEnvironmentVariableA("PW_VK_BATCH_FALLBACK_PROFILE",env,sizeof(env))==1&&env[0]=='1';
 enabled=GetEnvironmentVariableA("PW_VK_BATCH",env,sizeof(env))==1&&env[0]=='1'&&!pw_vk_stream_environment_unsafe();
 if(!read_opcode_mask())enabled=FALSE; /* Invalid explicit masks fail closed. */
 if(!enabled)return TRUE;
 InitializeCriticalSection(&gate);InitializeCriticalSection(&registry_gate);InitializeCriticalSection(&metadata_gate);tls=TlsAlloc();
 pw_vk_spsc_sequence_init(&stream_sequence);
 scratch=heap_alloc(PW_VK_BATCH_SCRATCH);
 enabled=tls!=TLS_OUT_OF_INDEXES&&scratch;
 /* Diagnostics are independently opt-in; FPS confirmation leaves them off. */
 return TRUE;
}
static void enter(void){EnterCriticalSection(&gate);if(depth&&owner==GetCurrentThreadId())fatal();owner=GetCurrentThreadId();depth=1;}
static void leave(void){depth=0;owner=0;LeaveCriticalSection(&gate);}
static struct producer *producer(void)
{
 struct producer *p=TlsGetValue(tls),*s;unsigned count=0;
 if(p)return p;
 if(!(p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p))))return NULL;
 if(pw_vk_spsc_init(&p->stream,p->arena,sizeof(p->arena))){heap_free(p);return NULL;}
 EnterCriticalSection(&registry_gate);
 for(s=registry.streams;s;s=s->next)count++;
 if(count>=PW_VK_BATCH_SCRATCH/PW_VK_BATCH_ARENA-1||!TlsSetValue(tls,p)){
  LeaveCriticalSection(&registry_gate);heap_free(p);return NULL;
 }
 p->next=registry.streams;registry.streams=p;
 LeaveCriticalSection(&registry_gate);return p;
}
/* Only the drain owner reclaims nodes. Registration holds this short lock, but
 * neither ordinary append nor driver replay does. */
static void reclaim(void)
{
 struct producer **link,*p;
 EnterCriticalSection(&registry_gate);
 for(link=&registry.streams;(p=*link);){
  if(InterlockedCompareExchange(&p->retired,0,0)&&
     atomic_load_explicit(&p->stream.read,memory_order_relaxed)==atomic_load_explicit(&p->stream.write,memory_order_acquire)){
   *link=p->next;heap_free(p);
  }else link=&p->next;
 }
 LeaveCriticalSection(&registry_gate);
}
static size_t producers(struct producer **list)
{
 struct producer *p;size_t n=0;
 EnterCriticalSection(&registry_gate);
 for(p=registry.streams;p;p=p->next)list[n++]=p;
 LeaveCriticalSection(&registry_gate);return n;
}
static void collect(size_t *bytes,size_t *records)
{
 struct producer *list[PW_VK_BATCH_SCRATCH/PW_VK_BATCH_ARENA];
 uint64_t marker=pw_vk_spsc_marker(&stream_sequence);size_t n=producers(list),i;
 *bytes=*records=0;
 while(next_replay<=marker){
  BOOL found=FALSE;
  for(i=0;i<n;i++){
   size_t used;int status=pw_vk_spsc_take(&list[i]->stream,next_replay,scratch+*bytes,PW_VK_BATCH_SCRATCH-*bytes,&used);
   if(status==PW_VK_STREAM_PENDING)continue;
   if(status)fatal();
   *bytes+=used;(*records)++;next_replay++;found=TRUE;break;
  }
  if(!found)SwitchToThread(); /* A reserved producer has not published yet. */
 }
 enqueued_total=marker; /* Snapshot counts exactly the accepted prefix. */
}
static void quiesce(void)
{
 struct producer *list[PW_VK_BATCH_SCRATCH/PW_VK_BATCH_ARENA];size_t n,i;
 InterlockedExchange(&quiescing,1);n=producers(list);
 for(i=0;i<n;i++)while(InterlockedCompareExchange(&list[i]->publishing,0,0))SwitchToThread();
}
void pw_vk_batch_thread_detach(void)
{
 struct producer *p;if(tls==TLS_OUT_OF_INDEXES)return;p=TlsGetValue(tls);
 if(p){/* No mutex or driver call under the loader lock. */TlsSetValue(tls,NULL);InterlockedExchange(&p->retired,1);}
}
static NTSTATUS raw_call(unsigned int code,void *args){if(stats_enabled)InterlockedIncrement64((LONG64 *)&crossings_total);return WINE_UNIX_CALL(code,args);}
static NTSTATUS flush_call(unsigned int code,void *args)
{
 size_t bytes=0,records=0;struct pw_vk_batch_params p;NTSTATUS status;
 collect(&bytes,&records);
 if(!bytes){status=code==unix_count?STATUS_SUCCESS:raw_call(code,args);pw_vk_retirement_drain(&retirement,free,heap_free);reclaim();return status;}
 p.version=PW_VK_BATCH_VERSION;p.batch=(UINT_PTR)scratch;p.bytes=bytes;p.code=code;p.args=(UINT_PTR)args;p.status=STATUS_SUCCESS;
 status=raw_call(unix_pw_vk_batch,&p);if(status)fatal();
 dispatches_total++;records_total+=records;if(code!=unix_count)piggyback_total++;
 pw_vk_retirement_drain(&retirement,free,heap_free);reclaim();return p.status;
}
/* Manual loader wrappers unlink PE lists immediately, but queued native thunks
 * still dereference each client-object prefix. Release only after global replay
 * completes. A concurrent flush may already have completed before this hook. */
void pw_vk_batch_retire_free(void *object)
{
 BOOL pending;
 if(!object)return;
 if(!enabled||InterlockedCompareExchange(&sticky_disabled,0,0)){free(object);return;}
 enter();
 pending=next_replay<=pw_vk_spsc_marker(&stream_sequence);
 if(!pending)free(object);
 else if(!pw_vk_retirement_add(&retirement,object,heap_alloc)){
  /* Node allocation failure preserves lifetime by completing replay first. */
  flush_call(unix_count,NULL);free(object);
 }
 leave();
}
static void template_created_locked(unsigned int code,void *args)
{
 const VkDescriptorUpdateTemplateCreateInfo *info;VkDescriptorUpdateTemplate handle;VkDevice device;VkResult result;
 struct pw_vk_template_entry *entries;size_t i;
 if(code==unix_vkCreateDescriptorUpdateTemplate){struct vkCreateDescriptorUpdateTemplate_params *p=args;info=p->pCreateInfo;result=p->result;handle=result==VK_SUCCESS&&p->pDescriptorUpdateTemplate?*p->pDescriptorUpdateTemplate:0;device=p->device;}
 else if(code==unix_vkCreateDescriptorUpdateTemplateKHR){struct vkCreateDescriptorUpdateTemplateKHR_params *p=args;info=p->pCreateInfo;result=p->result;handle=result==VK_SUCCESS&&p->pDescriptorUpdateTemplate?*p->pDescriptorUpdateTemplate:0;device=p->device;}
 else return;
 if(result!=VK_SUCCESS||!info)return;
 pw_vk_template_remove(&templates,&allocator,(UINT_PTR)device,handle);
 if(info->descriptorUpdateEntryCount>4096||(info->descriptorUpdateEntryCount&&!info->pDescriptorUpdateEntries))return;
 entries=heap_alloc((size_t)info->descriptorUpdateEntryCount*sizeof(*entries));if(!entries&&info->descriptorUpdateEntryCount)return;
 for(i=0;i<info->descriptorUpdateEntryCount;i++){entries[i].type=info->pDescriptorUpdateEntries[i].descriptorType;entries[i].count=info->pDescriptorUpdateEntries[i].descriptorCount;entries[i].offset=info->pDescriptorUpdateEntries[i].offset;entries[i].stride=info->pDescriptorUpdateEntries[i].stride;}
 pw_vk_template_register(&templates,&allocator,(UINT_PTR)device,handle,1,info->pNext!=NULL,info->flags,info->templateType,entries,info->descriptorUpdateEntryCount);heap_free(entries);
}
static void retire_template_locked(unsigned int code,void *args)
{
 if(code==unix_vkDestroyDescriptorUpdateTemplate){struct vkDestroyDescriptorUpdateTemplate_params *p=args;pw_vk_template_remove(&templates,&allocator,(UINT_PTR)p->device,p->descriptorUpdateTemplate);}
 else if(code==unix_vkDestroyDescriptorUpdateTemplateKHR){struct vkDestroyDescriptorUpdateTemplateKHR_params *p=args;pw_vk_template_remove(&templates,&allocator,(UINT_PTR)p->device,p->descriptorUpdateTemplate);}
 else if(code==unix_vkDestroyDevice){struct vkDestroyDevice_params *p=args;pw_vk_template_remove_device(&templates,&allocator,(UINT_PTR)p->device);}
}
static void template_created(unsigned code,void *args)
{
 EnterCriticalSection(&metadata_gate);template_created_locked(code,args);LeaveCriticalSection(&metadata_gate);
}
static void retire_template(unsigned code,void *args)
{
 if(code!=unix_vkDestroyDescriptorUpdateTemplate&&code!=unix_vkDestroyDescriptorUpdateTemplateKHR&&code!=unix_vkDestroyDevice)return;
 EnterCriticalSection(&metadata_gate);retire_template_locked(code,args);LeaveCriticalSection(&metadata_gate);
}
static VkDevice template_device(unsigned code,const void *args)
{
 VkCommandBuffer buffer;
 switch(code){
 case unix_vkUpdateDescriptorSetWithTemplate:return ((const struct vkUpdateDescriptorSetWithTemplate_params *)args)->device;
 case unix_vkUpdateDescriptorSetWithTemplateKHR:return ((const struct vkUpdateDescriptorSetWithTemplateKHR_params *)args)->device;
 case unix_vkCmdPushDescriptorSetWithTemplate:buffer=((const struct vkCmdPushDescriptorSetWithTemplate_params *)args)->commandBuffer;break;
 case unix_vkCmdPushDescriptorSetWithTemplateKHR:buffer=((const struct vkCmdPushDescriptorSetWithTemplateKHR_params *)args)->commandBuffer;break;
 case unix_vkCmdPushDescriptorSetWithTemplate2:buffer=((const struct vkCmdPushDescriptorSetWithTemplate2_params *)args)->commandBuffer;break;
 case unix_vkCmdPushDescriptorSetWithTemplate2KHR:buffer=((const struct vkCmdPushDescriptorSetWithTemplate2KHR_params *)args)->commandBuffer;break;
 default:return NULL;
 }
 return buffer?buffer->device:NULL;
}
static int template_snapshot(void *device,uint64_t handle,const void *data,void *wire,size_t capacity,size_t *written)
{
 int result;EnterCriticalSection(&metadata_gate);result=pw_vk_template_snapshot(&templates,(UINT_PTR)device,0,handle,data,1,wire,capacity,written);LeaveCriticalSection(&metadata_gate);return result;
}
static uint32_t call_opcode(unsigned int code)
{
 switch(code){
 case unix_vkUpdateDescriptorSetWithTemplate:return PW_VK_UPDATE_TEMPLATE;
 case unix_vkCmdDrawIndexed:return PW_VK_DRAW_INDEXED;
 case unix_vkCmdBindDescriptorSets:return PW_VK_BIND_DESCRIPTORS;
 case unix_vkCmdBindPipeline:return PW_VK_BIND_PIPELINE;
 case unix_vkCmdBindVertexBuffers2:return PW_VK_BIND_VERTEX2;
 case unix_vkCmdBindIndexBuffer2KHR:case unix_vkCmdBindIndexBuffer:return PW_VK_BIND_INDEX;
 case unix_vkCmdPushConstants:return PW_VK_PUSH_CONSTANTS;
 default:return 0;
 }
}
static int encode(unsigned int code,void *args,unsigned char *wire,size_t *written,uint32_t *opcode)
{
 size_t n=0;int encoded=0;uint32_t op=0,selected=call_opcode(code);
 if(!selected||!(opcode_mask&(1u<<(selected-1)))){*written=0;*opcode=0;return 0;}
 switch(code){
 case unix_vkCmdDrawIndexed:{const struct vkCmdDrawIndexed_params *p=args;op=PW_VK_DRAW_INDEXED;encoded=pw_vk_wire_draw(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->indexCount,p->instanceCount,p->firstIndex,p->vertexOffset,p->firstInstance,&n);break;}
 case unix_vkCmdBindPipeline:{const struct vkCmdBindPipeline_params *p=args;op=PW_VK_BIND_PIPELINE;encoded=pw_vk_wire_pipeline(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->pipelineBindPoint,p->pipeline,&n);break;}
 case unix_vkCmdBindIndexBuffer2KHR:{const struct vkCmdBindIndexBuffer2KHR_params *p=args;op=PW_VK_BIND_INDEX;encoded=pw_vk_wire_index(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->buffer,p->offset,p->size,p->indexType,1,&n);break;}
 case unix_vkCmdBindIndexBuffer:{const struct vkCmdBindIndexBuffer_params *p=args;op=PW_VK_BIND_INDEX;encoded=pw_vk_wire_index(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->buffer,p->offset,0,p->indexType,0,&n);break;}
 case unix_vkCmdBindDescriptorSets:{const struct vkCmdBindDescriptorSets_params *p=args;op=PW_VK_BIND_DESCRIPTORS;encoded=pw_vk_wire_descriptors(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->pipelineBindPoint,p->layout,p->firstSet,p->descriptorSetCount,(const uint64_t *)p->pDescriptorSets,p->dynamicOffsetCount,p->pDynamicOffsets,&n);break;}
 case unix_vkCmdBindVertexBuffers2:{const struct vkCmdBindVertexBuffers2_params *p=args;op=PW_VK_BIND_VERTEX2;encoded=pw_vk_wire_vertex2(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->firstBinding,p->bindingCount,(const uint64_t *)p->pBuffers,p->pOffsets,p->pSizes,p->pStrides,&n);break;}
 case unix_vkUpdateDescriptorSetWithTemplate:{const struct vkUpdateDescriptorSetWithTemplate_params *p=args;op=PW_VK_UPDATE_TEMPLATE;EnterCriticalSection(&metadata_gate);encoded=pw_vk_template_snapshot(&templates,(uint32_t)(uintptr_t)p->device,p->descriptorSet,p->descriptorUpdateTemplate,p->pData,1,wire,4096,&n);LeaveCriticalSection(&metadata_gate);break;}
 case unix_vkCmdPushConstants:{const struct vkCmdPushConstants_params *p=args;op=PW_VK_PUSH_CONSTANTS;encoded=pw_vk_wire_push_constants(wire,4096,(uint32_t)(uintptr_t)p->commandBuffer,p->layout,p->stageFlags,p->offset,p->size,p->pValues,&n);break;}
 default:break;
 }
 *written=n;*opcode=op;return encoded;
}

/* Counts are deltas over present ordinals, not rendered-frame or FPS claims.
 * The gate stays held; no callbacks, allocations or additional Unix calls. */
static void fallback_snapshot(UINT64 ordinal)
{
 unsigned top[8],count=0,i,j,k;UINT64 delta,total=0;
 if(!fallback_profile||!enabled||sticky_disabled||ordinal-fallback_report_present<300)return;
 for(i=0;i<unix_count;i++){
  delta=fallback_counts[i]-fallback_reported[i];total+=delta;
  if(!delta)continue;
  for(j=0;j<count;j++)if(delta>fallback_counts[top[j]]-fallback_reported[top[j]])break;
  if(j>=8)continue;
  if(count<8)count++;
  for(k=count-1;k>j;k--)top[k]=top[k-1];
  top[j]=i;
 }
 WINE_MESSAGE("PW_VK_FALLBACK version=1 scope=wine32_winevulkan_intercepted_process start_present=%llu end_present=%llu calls=%llu functions=%u top=%u\n",fallback_report_present,ordinal,total,(unsigned)unix_count,count);
 for(j=0;j<count;j++){
  i=top[j];
  WINE_MESSAGE("PW_VK_FALLBACK_TOP version=1 end_present=%llu rank=%u code=%u function=%s calls=%llu cumulative=%llu\n",ordinal,j+1,i,pw_vk_function_names[i],fallback_counts[i]-fallback_reported[i],fallback_counts[i]);
 }
 memcpy(fallback_reported,fallback_counts,sizeof(fallback_counts));fallback_report_present=ordinal;
}
static void snapshot(unsigned int code,void *args)
{
 if(stats_enabled&&code==unix_vkQueuePresentKHR){
  struct vkQueuePresentKHR_params *q=args;UINT64 ordinal=InterlockedIncrement64((LONG64 *)&present);
  WINE_MESSAGE("PW_VK_BATCH version=1 scope=process tid=%lu present=%llu result=%d enabled=%u negotiated=%u records=%llu enqueued=%llu batch_dispatches=%llu piggybacks=%llu standalone_flushes=%llu arena_full=%llu fallback=%llu wine_unix_crossings=%llu\n",GetCurrentThreadId(),ordinal,q->result,enabled&&!sticky_disabled,negotiated,records_total,enqueued_total,dispatches_total,piggyback_total,dispatches_total-piggyback_total,full_total,fallback_total,InterlockedCompareExchange64((LONG64 *)&crossings_total,0,0));
  fallback_snapshot(ordinal);
 }
}
NTSTATUS pw_vk_batch_call(unsigned int code,void *args)
{
 unsigned char *wire;size_t bytes;uint32_t opcode;struct producer *p;NTSTATUS status;int appended;
 InitOnceExecuteOnce(&once,initialize,NULL,NULL);
 /* Init and availability calls precede capability negotiation; old Unix never
  * receives the new table index. Disabled64 builds retain original macro. */
 if(!enabled||InterlockedCompareExchange(&sticky_disabled,0,0)){status=raw_call(code,args);snapshot(code,args);return status;}
 /* The replay gate is not part of the normal producer path. */
 if(negotiated&&!pw_vk_stream_call_unsafe(code,args)&&!pw_vk_batch_allocator(code,args)&&(p=producer())){
  InterlockedExchange(&p->publishing,1);
  if(!InterlockedCompareExchange(&quiescing,0,0)){
   wire=p->wire;bytes=0;opcode=0;
   if(!encode(code,args,wire,&bytes,&opcode)&&opcode_mask==0x7f&&
      pw_vk_generated_encode_templates(code,args,wire+4,PW_VK_BATCH_ARENA-PW_VK_STREAM_HEADER-4,&bytes,template_snapshot,template_device(code,args))){
    memcpy(wire,&code,4);bytes+=4;opcode=PW_VK_BATCH_GENERATED_OPCODE;
   }
   appended=bytes?pw_vk_spsc_append(&stream_sequence,&p->stream,opcode,wire,bytes):PW_VK_STREAM_INVALID;
   if(appended==PW_VK_STREAM_OK){
    retire_template(code,args);InterlockedExchange(&p->publishing,0);return STATUS_SUCCESS;
   }
   InterlockedExchange(&p->publishing,0);
   if(appended==PW_VK_STREAM_FULL){
    /* Release publisher state before waiting for a drain/disable owner. */
    enter();full_total++;flush_call(unix_count,NULL);leave();
    return pw_vk_batch_call(code,args);
   }
   if(appended!=PW_VK_STREAM_INVALID)fatal();
  }else InterlockedExchange(&p->publishing,0);
 }
 enter();
 if(InterlockedCompareExchange(&sticky_disabled,0,0)){leave();status=raw_call(code,args);snapshot(code,args);return status;}
 if(pw_vk_stream_call_unsafe(code,args)||pw_vk_batch_allocator(code,args)){
  quiesce();if(negotiated)flush_call(unix_count,NULL);
  InterlockedExchange(&sticky_disabled,1);leave();status=raw_call(code,args);snapshot(code,args);return status;
 }
 fallback_total++;
 if(fallback_profile&&code<unix_count)fallback_counts[code]++;
 if(pw_vk_stream_progress_call(code)){
  /* Complete owned replay before unlocking. Never piggyback a driver wait on
   * shared scratch: another thread must be able to drain and signal it. */
  if(negotiated)flush_call(unix_count,NULL);
  leave();status=raw_call(code,args);
  /* All other progress operations have no cache/lifetime posthooks. Present
   * diagnostics read non-atomic cumulative totals briefly under the gate. */
  if(stats_enabled&&code==unix_vkQueuePresentKHR){enter();snapshot(code,args);leave();}
  return status;
 }
 status=negotiated?flush_call(code,args):raw_call(code,args);
 retire_template(code,args);template_created(code,args);
 if(code==unix_vkCreateInstance&&!status){struct vkCreateInstance_params *q=args;if(q->result==VK_SUCCESS&&q->pInstance&&*q->pInstance){struct is_available_instance_function_params cap={*q->pInstance,PW_VK_BATCH_NAME};negotiated=raw_call(unix_is_available_instance_function,&cap)==PW_VK_BATCH_CAPABILITY;}}
 snapshot(code,args);
 leave();return status;
}
#endif
