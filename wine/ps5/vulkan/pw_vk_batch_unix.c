/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Unix-side replay executes entirely with normal host FS. */
#if 0
#pragma makedep unix
#endif
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "vulkan_private.h"
#include "pw_vk_wire.h"
#include "pw_vk_batch.h"
#include "pw_vk_codec.h"
#include "pw_vk_command_stream.h"
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
struct replay_context { void *arena; unsigned version; };
static int generated_record(struct replay_context *ctx,const struct pw_vk_stream_record *r,void **params)
{
 unsigned code;
 if(ctx->version!=PW_VK_BATCH_VERSION||r->payload_bytes<4||r->payload_bytes>PW_VK_CODEC_MAX_BYTES)return 0;
 memcpy(&code,r->payload,4);
 return code<unix_pw_vk_batch&&pw_vk_generated_decode(code,r->payload+4,r->payload_bytes-4,ctx->arena,PW_VK_CODEC_DECODE_BYTES,params);
}
static int preflight(void *context,const struct pw_vk_stream_record *r)
{
 struct replay_context *ctx=context;void *params;
 if(r->opcode==PW_VK_BATCH_GENERATED_OPCODE)return !generated_record(ctx,r,&params);
 return !(r->payload_bytes<=4096&&pw_vk_wire_validate(r->opcode,r->payload,r->payload_bytes));
}
static int replay(void *context,const struct pw_vk_stream_record *r)
{
 struct replay_context *ctx=context;void *params;unsigned code;
 if(r->opcode!=PW_VK_BATCH_GENERATED_OPCODE)return !pw_wine_vk_replay(r->opcode,r->payload,r->payload_bytes);
 if(!generated_record(ctx,r,&params))return 1;
 memcpy(&code,r->payload,4);return pw_vk_batch_dispatch_native(code,params)!=STATUS_SUCCESS;
}
NTSTATUS pw_vk_batch_unix(void *args)
{
 struct pw_vk_batch_params *p=args;size_t completed;struct replay_context ctx;NTSTATUS status=STATUS_SUCCESS;
 if((p->version!=PW_VK_BATCH_VERSION&&p->version!=PW_VK_BATCH_LEGACY_VERSION)||p->bytes>PW_VK_BATCH_SCRATCH||p->code>unix_count||p->code==unix_pw_vk_batch)return STATUS_INVALID_PARAMETER;
 ctx.version=p->version;ctx.arena=malloc(PW_VK_CODEC_DECODE_BYTES);if(!ctx.arena)return STATUS_NO_MEMORY;
 if(pw_vk_stream_replay(UlongToPtr(p->batch),p->bytes,preflight,&ctx,&completed)!=PW_VK_STREAM_OK)status=STATUS_INVALID_PARAMETER;
 else if(pw_vk_stream_replay(UlongToPtr(p->batch),p->bytes,replay,&ctx,&completed)!=PW_VK_STREAM_OK)status=STATUS_UNSUCCESSFUL;
 free(ctx.arena);if(status)return status;
 p->status=STATUS_SUCCESS;
 /* unix_count is the private flush-only sentinel. This check occurs before
  * any dispatch; ordinary perf-critical thunks really return void. */
 if(p->code!=unix_count){
  p->status=pw_vk_batch_dispatch(p->code,UlongToPtr(p->args));
 }
 return STATUS_SUCCESS;
}
