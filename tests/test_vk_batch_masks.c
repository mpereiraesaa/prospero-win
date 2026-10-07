/* Actual PE runtime and Win32 APIs, mocked Unix boundary; no console. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vulkan_loader.h"
#include "pw_vk_batch.h"
#include "pw_vk_command_stream.h"
#include "pw_vk_wire.h"
static NTSTATUS mock_call(unsigned int,void *);
#undef WINE_UNIX_CALL
#define WINE_UNIX_CALL(code,args) mock_call(code,args)
#undef ERR
#define ERR(...) fprintf(stderr,__VA_ARGS__)
#undef WINE_MESSAGE
#define WINE_MESSAGE(...) fprintf(stderr,__VA_ARGS__)
#include "pw_vk_batch_pe.c"
static unsigned raw[8],played[8],trace[32],used;
static int effect(void *unused,const struct pw_vk_stream_record *r)
{
 (void)unused;assert(pw_vk_wire_validate(r->opcode,r->payload,r->payload_bytes));assert(r->opcode>=1&&r->opcode<=7);played[r->opcode]++;trace[used++]=r->opcode;
 if(r->opcode==PW_VK_UPDATE_TEMPLATE){assert(pw_vk_wire_u64(r->payload+32)==0x123456789abcdef0ULL);assert(pw_vk_wire_u64(r->payload+40)==11);assert(pw_vk_wire_u64(r->payload+48)==99);}
 if(r->opcode==PW_VK_DRAW_INDEXED)assert(pw_vk_wire_u32(r->payload+4)==7);
 if(r->opcode==PW_VK_PUSH_CONSTANTS)assert(pw_vk_wire_u32(r->payload+24)==0x10203040);
 return 0;
}
static NTSTATUS original(unsigned int code,void *args)
{
 unsigned op=call_opcode(code);
 if(op){raw[op]++;trace[used++]=0x100|op;}
 if(code==unix_vkCreateInstance){struct vkCreateInstance_params *p=args;*p->pInstance=(VkInstance)(uintptr_t)7;p->result=VK_SUCCESS;}
 if(code==unix_is_available_instance_function){struct is_available_instance_function_params *p=args;return !strcmp(p->name,PW_VK_BATCH_NAME)?PW_VK_BATCH_CAPABILITY:0;}
 if(code==unix_vkCreateDescriptorUpdateTemplate){struct vkCreateDescriptorUpdateTemplate_params *p=args;*p->pDescriptorUpdateTemplate=42;p->result=VK_SUCCESS;}
 return STATUS_SUCCESS;
}
static NTSTATUS mock_call(unsigned int code,void *args)
{
 if(code==unix_pw_vk_batch){struct pw_vk_batch_params *p=args;size_t done;assert(pw_vk_stream_replay((void *)(uintptr_t)p->batch,p->bytes,effect,NULL,&done)==PW_VK_STREAM_OK);p->status=p->code==unix_count?STATUS_SUCCESS:original(p->code,(void *)(uintptr_t)p->args);return STATUS_SUCCESS;}
 return original(code,args);
}
int main(int argc,char **argv)
{
 VkInstance instance;VkInstanceCreateInfo info={0};struct vkCreateInstance_params create={0};VkDescriptorUpdateTemplate handle;VkDescriptorUpdateTemplateEntry entry={0};VkDescriptorUpdateTemplateCreateInfo ti={0};struct vkCreateDescriptorUpdateTemplate_params tc={0};VkDescriptorBufferInfo data={0x123456789abcdef0ULL,11,99};struct vkUpdateDescriptorSetWithTemplate_params update={0};struct vkCmdDrawIndexed_params draw={0};struct vkCmdBindDescriptorSets_params ds={0};struct vkCmdBindPipeline_params pipeline={0};struct vkCmdBindVertexBuffers2_params vb={0};struct vkCmdBindIndexBuffer_params ib={0};struct vkCmdPushConstants_params pc={0};struct vkEndCommandBuffer_params end={0};VkDescriptorSet set=17;VkBuffer buffer=18;VkDeviceSize offset=19,size=20,stride=21;uint32_t dynamic=22,value=0x10203040;unsigned i,count=0;uint32_t expected;UINT64 before;
 assert(argc==3);expected=(uint32_t)strtoul(argv[2],NULL,0);
 SetEnvironmentVariableA("PW_VK_BATCH","1");SetEnvironmentVariableA("PW_VK_BATCH_STATS","1");SetEnvironmentVariableA("PW_VK_BATCH_MASK",strcmp(argv[1],"unset")?argv[1]:NULL);
 create.pCreateInfo=&info;create.pInstance=&instance;assert(pw_vk_batch_call(unix_vkCreateInstance,&create)==0);
 if(expected==0xff){assert(!enabled&&!negotiated&&tls==TLS_OUT_OF_INDEXES);puts("PASS invalid mask fails closed");return 0;}
 assert(enabled&&negotiated&&opcode_mask==expected);
 entry.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;entry.descriptorCount=1;entry.stride=sizeof(data);ti.descriptorUpdateEntryCount=1;ti.pDescriptorUpdateEntries=&entry;ti.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;tc.device=(VkDevice)(uintptr_t)11;tc.pCreateInfo=&ti;tc.pDescriptorUpdateTemplate=&handle;assert(pw_vk_batch_call(unix_vkCreateDescriptorUpdateTemplate,&tc)==0);before=crossings_total;
 update.device=tc.device;update.descriptorSet=set;update.descriptorUpdateTemplate=handle;update.pData=(expected&(1u<<(PW_VK_UPDATE_TEMPLATE-1)))?&data:(const void *)(uintptr_t)1;assert(pw_vk_batch_call(unix_vkUpdateDescriptorSetWithTemplate,&update)==0);memset(&data,0xee,sizeof(data));
 draw.commandBuffer=(VkCommandBuffer)(uintptr_t)9;draw.indexCount=7;draw.instanceCount=1;assert(pw_vk_batch_call(unix_vkCmdDrawIndexed,&draw)==0);memset(&draw,0xee,sizeof(draw));
 ds.commandBuffer=(VkCommandBuffer)(uintptr_t)9;ds.layout=23;ds.descriptorSetCount=1;ds.pDescriptorSets=&set;ds.dynamicOffsetCount=1;ds.pDynamicOffsets=&dynamic;assert(pw_vk_batch_call(unix_vkCmdBindDescriptorSets,&ds)==0);set=dynamic=0;
 pipeline.commandBuffer=ds.commandBuffer;pipeline.pipeline=24;assert(pw_vk_batch_call(unix_vkCmdBindPipeline,&pipeline)==0);
 vb.commandBuffer=ds.commandBuffer;vb.bindingCount=1;vb.pBuffers=&buffer;vb.pOffsets=&offset;vb.pSizes=&size;vb.pStrides=&stride;assert(pw_vk_batch_call(unix_vkCmdBindVertexBuffers2,&vb)==0);buffer=offset=size=stride=0;
 ib.commandBuffer=ds.commandBuffer;ib.buffer=25;ib.indexType=VK_INDEX_TYPE_UINT16;assert(pw_vk_batch_call(unix_vkCmdBindIndexBuffer,&ib)==0);
 pc.commandBuffer=ds.commandBuffer;pc.layout=26;pc.stageFlags=VK_SHADER_STAGE_VERTEX_BIT;pc.size=sizeof(value);pc.pValues=(expected&(1u<<(PW_VK_PUSH_CONSTANTS-1)))?&value:(const void *)(uintptr_t)1;assert(pw_vk_batch_call(unix_vkCmdPushConstants,&pc)==0);value=0;
 end.commandBuffer=ds.commandBuffer;assert(pw_vk_batch_call(unix_vkEndCommandBuffer,&end)==0);
 assert(used==7);
 for(i=1;i<=7;i++){unsigned selected=!!(expected&(1u<<(i-1)));assert(played[i]==selected&&raw[i]==!selected);count+=selected;}
 {const unsigned order[]={PW_VK_UPDATE_TEMPLATE,PW_VK_DRAW_INDEXED,PW_VK_BIND_DESCRIPTORS,PW_VK_BIND_PIPELINE,PW_VK_BIND_VERTEX2,PW_VK_BIND_INDEX,PW_VK_PUSH_CONSTANTS};
  for(i=0;i<7;i++)assert(trace[i]==((expected&(1u<<(order[i]-1)))?order[i]:(0x100|order[i])));
 }
 assert(enqueued_total==count&&records_total==count);assert(crossings_total-before==8-count);
 puts("PASS mask selection, all seven categories, owned payloads, fallback ordering and exact crossing stats");return 0;
}
