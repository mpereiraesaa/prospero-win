/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Unix-side replay executes entirely with normal host FS. */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "vulkan_private.h"
#include "pw_vk_wire.h"
#include "pw_vk_batch.h"
#include "pw_vk_codec.h"
#include "pw_vk_command_stream.h"
#include "pw_vk_replay.h"
#include "pw_vk_async.h"
#include "pw_vk_replay_dispatch.h"
int pw_wine_vk_replay(uint32_t op,const void *wire,size_t bytes)
{
 const unsigned char *p=wire; struct vulkan_command_buffer *cb;
 uint32_t count,i,dc,flags; uint64_t storage[512],*a=storage; uint32_t *dynamic=NULL;
 if(bytes>sizeof(storage)||!pw_vk_wire_validate(op,wire,bytes))return 0;
 if(op==PW_VK_UPDATE_TEMPLATE){struct vulkan_device *device=vulkan_device_from_handle((VkDevice)UlongToPtr(pw_vk_wire_u32(p)));memcpy(storage,p+32,bytes-32);device->p_vkUpdateDescriptorSetWithTemplate(device->host.device,pw_vk_wire_u64(p+8),pw_vk_wire_u64(p+16),storage);return 1;}
 cb=vulkan_command_buffer_from_handle((VkCommandBuffer)UlongToPtr(pw_vk_wire_u32(p)));
 switch(op){
 case PW_VK_DRAW_INDEXED:cb->device->p_vkCmdDrawIndexed(cb->host.command_buffer,pw_vk_wire_u32(p+4),pw_vk_wire_u32(p+8),pw_vk_wire_u32(p+12),(int32_t)pw_vk_wire_u32(p+16),pw_vk_wire_u32(p+20));break;
 case PW_VK_PUSH_CONSTANTS:cb->device->p_vkCmdPushConstants(cb->host.command_buffer,pw_vk_wire_u64(p+8),pw_vk_wire_u32(p+4),pw_vk_wire_u32(p+16),pw_vk_wire_u32(p+20),p+24);break;
 case PW_VK_BIND_PIPELINE:cb->device->p_vkCmdBindPipeline(cb->host.command_buffer,pw_vk_wire_u32(p+4),pw_vk_wire_u64(p+8));break;
 case PW_VK_BIND_INDEX:if(pw_vk_wire_u32(p+32))cb->device->p_vkCmdBindIndexBuffer2KHR(cb->host.command_buffer,pw_vk_wire_u64(p+8),pw_vk_wire_u64(p+16),pw_vk_wire_u64(p+24),pw_vk_wire_u32(p+4));else cb->device->p_vkCmdBindIndexBuffer(cb->host.command_buffer,pw_vk_wire_u64(p+8),pw_vk_wire_u64(p+16),pw_vk_wire_u32(p+4));break;
 case PW_VK_BIND_DESCRIPTORS:
  count=pw_vk_wire_u32(p+20);dc=pw_vk_wire_u32(p+24);
  dynamic=(uint32_t *)(storage+count);
  for(i=0;i<count;i++)a[i]=pw_vk_wire_u64(p+32+(size_t)i*8);
  for(i=0;i<dc;i++)dynamic[i]=pw_vk_wire_u32(p+32+(size_t)count*8+(size_t)i*4);
  cb->device->p_vkCmdBindDescriptorSets(cb->host.command_buffer,pw_vk_wire_u32(p+4),pw_vk_wire_u64(p+8),pw_vk_wire_u32(p+16),count,(VkDescriptorSet *)a,dc,dynamic);break;
 case PW_VK_BIND_VERTEX2:
  count=pw_vk_wire_u32(p+8);flags=pw_vk_wire_u32(p+12);
  for(i=0;i<count;i++){a[i]=pw_vk_wire_u64(p+16+(size_t)i*32);a[count+i]=pw_vk_wire_u64(p+24+(size_t)i*32);a[2*count+i]=pw_vk_wire_u64(p+32+(size_t)i*32);a[3*count+i]=pw_vk_wire_u64(p+40+(size_t)i*32);}
  cb->device->p_vkCmdBindVertexBuffers2(cb->host.command_buffer,pw_vk_wire_u32(p+4),count,(VkBuffer *)a,(VkDeviceSize *)(count?a+count:NULL),flags&1?(VkDeviceSize *)(count?a+2*count:NULL):NULL,flags&2?(VkDeviceSize *)(count?a+3*count:NULL):NULL);break;
 default:return 0;
 }
 return 1;
}

extern NTSTATUS pw_vk_batch_dispatch(unsigned int,void *);
extern NTSTATUS pw_vk_batch_dispatch_native(unsigned int,void *);
/* Each entry owns its arena, including recursive and concurrent invocations.
 * Common records need no heap allocation; unusually large schemas retain the
 * existing bound and grow at most once across preflight and replay. */
static unsigned startup_trace,trace_jobs,trace_enqueues;
struct replay_context {
 uint64_t local[512];
 void *arena; size_t capacity; unsigned version,trace,trace_records,trace_job; uintptr_t trace_cb,trace_pool; NTSTATUS failure;
};
static int generated_record(struct replay_context *ctx,const struct pw_vk_stream_record *r,void **params)
{
 unsigned code;
 if((ctx->version!=PW_VK_BATCH_VERSION&&ctx->version!=PW_VK_BATCH_ASYNC_VERSION)||r->payload_bytes<4||r->payload_bytes>PW_VK_CODEC_MAX_BYTES)return 0;
 memcpy(&code,r->payload,4);
 if(code>=unix_pw_vk_batch)return 0;
 if(pw_vk_generated_decode(code,r->payload+4,r->payload_bytes-4,ctx->arena,ctx->capacity,params))return 1;
 if(ctx->arena!=ctx->local)return 0;
 /* A small-arena failure may be capacity or invalid input. Retry once at the
  * full bound so no valid schema loses coverage; malformed input still has no
  * effects because this happens during the complete semantic preflight. */
 if(!(ctx->arena=malloc(PW_VK_CODEC_DECODE_BYTES))){ctx->failure=STATUS_NO_MEMORY;return 0;}
 ctx->capacity=PW_VK_CODEC_DECODE_BYTES;
 return pw_vk_generated_decode(code,r->payload+4,r->payload_bytes-4,ctx->arena,ctx->capacity,params);
}
static int preflight(void *context,const struct pw_vk_stream_record *r)
{
 struct replay_context *ctx=context;void *params;
 if(r->opcode==PW_VK_BATCH_GENERATED_OPCODE)return !generated_record(ctx,r,&params);
 return !(r->payload_bytes<=4096&&pw_vk_wire_validate(r->opcode,r->payload,r->payload_bytes));
}
static int replay(void *context,const struct pw_vk_stream_record *r)
{
 struct replay_context *ctx=context;void *params;unsigned code=0,traced=0;int result;
 if(ctx->trace && ctx->trace_records<32){++ctx->trace_records;traced=1;}
 if(r->opcode==PW_VK_BATCH_GENERATED_OPCODE){
  if(!generated_record(ctx,r,&params))return 1;
  memcpy(&code,r->payload,4);
 }
 if(traced)fprintf(stderr,"PW_VK_REPLAY_TRACE event=dispatch_begin job=%u record=%u cb=%p pool=%p opcode=%u code=%u bytes=%u\n",ctx->trace_job,ctx->trace_records,(void *)ctx->trace_cb,(void *)ctx->trace_pool,r->opcode,code,r->payload_bytes);
 result=r->opcode==PW_VK_BATCH_GENERATED_OPCODE?pw_vk_batch_dispatch_native(code,params)!=STATUS_SUCCESS:!pw_wine_vk_replay(r->opcode,r->payload,r->payload_bytes);
 if(traced)fprintf(stderr,"PW_VK_REPLAY_TRACE event=dispatch_end job=%u record=%u cb=%p pool=%p opcode=%u code=%u result=%d\n",ctx->trace_job,ctx->trace_records,(void *)ctx->trace_cb,(void *)ctx->trace_pool,r->opcode,code,result);
 return result;
}
/* Admission protects lane ownership and synchronization snapshots. Workers
 * never acquire it or call Wine TLS helpers. Vulkan application synchronization
 * still owns command-buffer/pool use across external API callers. */
static pthread_mutex_t admission = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t worker_once = PTHREAD_ONCE_INIT;
static struct pw_vk_replay *workers;
static int startup_failed;
static unsigned report_enabled;
static uint64_t boundary_count;
static void context_init(struct replay_context *ctx,unsigned version)
{
 ctx->trace=0;ctx->version=version;ctx->arena=ctx->local;ctx->capacity=sizeof(ctx->local);ctx->failure=STATUS_SUCCESS;
}
static void context_free(struct replay_context *ctx)
{
 if(ctx->arena && ctx->arena!=ctx->local)free(ctx->arena);
}
static int worker_replay(void *lane_context,const void *data,size_t bytes)
{
 struct replay_context ctx;size_t completed;int result;
 context_init(&ctx,PW_VK_BATCH_VERSION);
 if(startup_trace && __atomic_load_n(&trace_jobs,__ATOMIC_RELAXED)<8){
  unsigned job=__atomic_fetch_add(&trace_jobs,1,__ATOMIC_RELAXED);
  if(job<8){ctx.trace=1;ctx.trace_job=job+1;ctx.trace_records=0;ctx.trace_cb=(uintptr_t)lane_context;
   ctx.trace_pool=(uintptr_t)((struct wine_cmd_buffer *)lane_context)->pool;}
 }
 result=pw_vk_stream_replay(data,bytes,replay,&ctx,&completed);
 context_free(&ctx);return result!=PW_VK_STREAM_OK;
}
static void initialize_workers(void)
{
 const char *value=getenv("PW_VK_REPLAY_THREADS"),*stats=getenv("PW_VK_BATCH_STATS");
 unsigned long count=2;char *end;const char *trace=getenv("PW_VK_REPLAY_TRACE");
 startup_trace=trace && !strcmp(trace,"1");
 if(startup_trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=initialize_begin\n");
 pthread_mutex_lock(&admission);
 report_enabled=stats && !strcmp(stats,"1");
 if(value){errno=0;count=strtoul(value,&end,10);if(errno || !*value || *end || count>PW_VK_REPLAY_MAX_WORKERS){startup_failed=1;goto done;}}
 if(!count)goto done;
 workers=pw_vk_replay_create((unsigned)count,2u*PW_VK_BATCH_SCRATCH,worker_replay);
 if(!workers)startup_failed=1;
 done:if(startup_trace)fprintf(stderr,"PW_VK_REPLAY_TRACE event=initialize_end workers=%lu failed=%d ready=%d\n",count,startup_failed,workers!=NULL);
 pthread_mutex_unlock(&admission);
}
static void must_complete(int status)
{
 /* Continuing a lifecycle operation after failed replay could free live driver
  * objects. Preserve the existing fatal batch failure contract on raw hooks. */
 if(status){fprintf(stderr,"PW_VK_REPLAY_FATAL status=%d\n",status);abort();}
}
void pw_vk_async_wait_buffer(VkCommandBuffer handle)
{
 struct wine_cmd_buffer *cb;
 pthread_mutex_lock(&admission);
 if(workers && handle){cb=wine_cmd_buffer_from_handle(handle);if(cb->replay_lane)must_complete(pw_vk_replay_wait(cb->replay_lane,pw_vk_replay_marker(cb->replay_lane)));}
 pthread_mutex_unlock(&admission);
}
void pw_vk_async_wait_buffer_pool(VkCommandBuffer handle)
{
 pthread_mutex_lock(&admission);
 if(workers && handle)must_complete(pw_vk_replay_wait_pool(workers,(uintptr_t)wine_cmd_buffer_from_handle(handle)->pool));
 pthread_mutex_unlock(&admission);
}
void pw_vk_async_wait_pool(VkCommandPool handle)
{
 pthread_mutex_lock(&admission);
 if(workers && handle)must_complete(pw_vk_replay_wait_pool(workers,(uintptr_t)wine_cmd_pool_from_handle(handle)));
 pthread_mutex_unlock(&admission);
}
void pw_vk_async_forget_buffer(VkCommandBuffer handle)
{
 struct wine_cmd_buffer *cb;
 pthread_mutex_lock(&admission);
 if(workers && handle){cb=wine_cmd_buffer_from_handle(handle);if(cb->replay_lane){must_complete(pw_vk_replay_lane_drop(cb->replay_lane));cb->replay_lane=NULL;}}
 pthread_mutex_unlock(&admission);
}
static void report_workers(void)
{
 struct pw_vk_replay_stats s;
 if(!workers || !report_enabled)return;
 pw_vk_replay_get_stats(workers,&s);
 fprintf(stderr,"PW_VK_REPLAY version=1 workers=%u jobs=%llu completed=%llu peak_active=%u owned=%zu peak_owned=%zu capacity_waits=%llu completion_waits=%llu error=%d worker0=%llu worker1=%llu worker2=%llu worker3=%llu\n",s.workers,(unsigned long long)s.submitted,(unsigned long long)s.completed,s.peak_active,s.owned_bytes,s.peak_owned_bytes,(unsigned long long)s.capacity_waits,(unsigned long long)s.completion_waits,s.callback_error,(unsigned long long)s.worker_jobs[0],(unsigned long long)s.worker_jobs[1],(unsigned long long)s.worker_jobs[2],(unsigned long long)s.worker_jobs[3]);
}
static void __attribute__((destructor)) stop_workers(void)
{
 pthread_mutex_lock(&admission);
 if(workers){report_workers();pw_vk_replay_destroy(workers);workers=NULL;}
 pthread_mutex_unlock(&admission);
}
struct schedule_context {
 struct replay_context decode;
 struct wine_cmd_buffer *pending_cb;
 const unsigned char *pending_data;
 size_t pending_bytes;
};
static int flush_pending(struct schedule_context *ctx)
{
 struct wine_cmd_buffer *cb=ctx->pending_cb;uint64_t ticket;int status;
 if(!cb)return 0;
 if(!cb->replay_lane)cb->replay_lane=pw_vk_replay_lane_create(workers,(uintptr_t)cb->pool,cb);
 if(!cb->replay_lane)return 1;
 if(startup_trace && trace_enqueues<8)fprintf(stderr,"PW_VK_REPLAY_TRACE event=enqueue_begin cb=%p pool=%p bytes=%zu\n",(void *)cb,(void *)cb->pool,ctx->pending_bytes);
 status=pw_vk_replay_enqueue(cb->replay_lane,ctx->pending_data,ctx->pending_bytes,&ticket);
 if(startup_trace && trace_enqueues<8){++trace_enqueues;fprintf(stderr,"PW_VK_REPLAY_TRACE event=enqueue_end cb=%p result=%d\n",(void *)cb,status);}
 ctx->pending_cb=NULL;ctx->pending_bytes=0;return status!=PW_VK_REPLAY_OK;
}
static int schedule_record(void *context,const struct pw_vk_stream_record *r)
{
 struct schedule_context *ctx=context;VkCommandBuffer handle=NULL;struct wine_cmd_buffer *cb;
 void *params;unsigned code;int status;
 if(r->opcode==PW_VK_BATCH_GENERATED_OPCODE){
  if(!generated_record(&ctx->decode,r,&params))return 1;
  memcpy(&code,r->payload,4);handle=pw_vk_replay_command_buffer(code,params);
 }else if(r->opcode!=PW_VK_UPDATE_TEMPLATE)handle=(VkCommandBuffer)UlongToPtr(pw_vk_wire_u32(r->payload));
 if(handle){
  cb=wine_cmd_buffer_from_handle(handle);
  if(ctx->pending_cb && ctx->pending_cb!=cb && flush_pending(ctx))return 1;
  if(!ctx->pending_cb){ctx->pending_cb=cb;ctx->pending_data=r->payload-PW_VK_STREAM_HEADER;}
  ctx->pending_bytes+=(PW_VK_STREAM_HEADER+(size_t)r->payload_bytes+7)&~(size_t)7;
  return 0;
 }
 /* Resource mutation and unreviewed commands retain synchronous global order.
  * Release admission around Wine dispatch: lifecycle hooks acquire it themselves. */
 if(flush_pending(ctx) || pw_vk_replay_wait_all(workers))return 1;
 pthread_mutex_unlock(&admission);status=replay(&ctx->decode,r);pthread_mutex_lock(&admission);
 return status;
}
/* These raw calls have their own scoped lifecycle barriers, or only observe /
 * advance queue/GPU progress. None mutates a resource used by worker recording. */
static int scoped_fallback(unsigned code)
{
 switch(code){
 case unix_vkBeginCommandBuffer:case unix_vkEndCommandBuffer:case unix_vkResetCommandBuffer:
 case unix_vkResetCommandPool:case unix_vkTrimCommandPool:case unix_vkTrimCommandPoolKHR:
 case unix_vkAllocateCommandBuffers:case unix_vkFreeCommandBuffers:case unix_vkDestroyCommandPool:
 case unix_vkQueueSubmit:case unix_vkQueueSubmit2:case unix_vkQueueSubmit2KHR:
 case unix_vkQueuePresentKHR:case unix_vkQueueWaitIdle:case unix_vkDeviceWaitIdle:
 case unix_vkWaitForFences:case unix_vkWaitSemaphores:case unix_vkWaitSemaphoresKHR:
 case unix_vkSignalSemaphore:case unix_vkSignalSemaphoreKHR:
 case unix_vkAcquireNextImageKHR:case unix_vkAcquireNextImage2KHR:
 case unix_vkGetFenceStatus:case unix_vkGetSemaphoreCounterValue:case unix_vkGetSemaphoreCounterValueKHR:
  return 1;
 default:return 0;
 }
}
NTSTATUS pw_vk_batch_unix(void *args)
{
 struct pw_vk_batch_params *p=args;size_t completed;struct schedule_context ctx;NTSTATUS status=STATUS_SUCCESS;
 if((p->version!=PW_VK_BATCH_VERSION&&p->version!=PW_VK_BATCH_LEGACY_VERSION&&p->version!=PW_VK_BATCH_ASYNC_VERSION)||p->bytes>PW_VK_BATCH_SCRATCH||p->code>unix_count+(p->version==PW_VK_BATCH_ASYNC_VERSION)||p->code==unix_pw_vk_batch)return STATUS_INVALID_PARAMETER;
 memset(&ctx,0,sizeof(ctx));context_init(&ctx.decode,p->version);
 if(pw_vk_stream_replay(UlongToPtr(p->batch),p->bytes,preflight,&ctx.decode,&completed)!=PW_VK_STREAM_OK){status=ctx.decode.failure?ctx.decode.failure:STATUS_INVALID_PARAMETER;goto done;}
 if(p->version==PW_VK_BATCH_ASYNC_VERSION)pthread_once(&worker_once,initialize_workers);
 pthread_mutex_lock(&admission);
 if(startup_failed){pthread_mutex_unlock(&admission);status=STATUS_NO_MEMORY;goto done;}
 if(workers && p->version==PW_VK_BATCH_ASYNC_VERSION){
  if(pw_vk_stream_replay(UlongToPtr(p->batch),p->bytes,schedule_record,&ctx,&completed)!=PW_VK_STREAM_OK || flush_pending(&ctx))status=STATUS_UNSUCCESSFUL;
  /* A flush-only call is also the PE callback-disable and retirement boundary.
   * It must complete all owned jobs before returning to that unchanged ABI. */
  if(!status && p->code!=unix_count+1 && !scoped_fallback(p->code) && pw_vk_replay_wait_all(workers))status=STATUS_UNSUCCESSFUL;
 }else{
  if(workers)must_complete(pw_vk_replay_wait_all(workers));
  pthread_mutex_unlock(&admission);
  if(pw_vk_stream_replay(UlongToPtr(p->batch),p->bytes,replay,&ctx.decode,&completed)!=PW_VK_STREAM_OK)status=STATUS_UNSUCCESSFUL;
  pthread_mutex_lock(&admission);
 }
 if((p->code==unix_count+1 || p->code==unix_vkQueuePresentKHR) && !(++boundary_count%1024))report_workers();
 pthread_mutex_unlock(&admission);
 if(!status){p->status=STATUS_SUCCESS;if(p->code<unix_count)p->status=pw_vk_batch_dispatch(p->code,UlongToPtr(p->args));}
 done:context_free(&ctx.decode);return status;
}
