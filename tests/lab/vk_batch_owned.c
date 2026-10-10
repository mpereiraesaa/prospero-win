/* Actual PE producer metadata, generated snapshots and deferred client frees. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vulkan_loader.h"
#include "pw_vk_batch.h"
#include "pw_vk_codec.h"
#include "pw_vk_command_stream.h"
#include "pw_vk_spsc.h"
#include "pw_vk_wire.h"
static NTSTATUS mock_call(unsigned int,void *);
#undef WINE_UNIX_CALL
#define WINE_UNIX_CALL(code,args) mock_call(code,args)
#undef ERR
#define ERR(...) fprintf(stderr,__VA_ARGS__)
#undef WINE_MESSAGE
#define WINE_MESSAGE(...) fprintf(stderr,__VA_ARGS__)
static int observed_append(struct pw_vk_spsc_sequence *,struct pw_vk_spsc *,uint32_t,const void *,uint32_t);
#define pw_vk_spsc_append observed_append
#include "pw_vk_batch_pe.c"
/* raw_call reaches the Unix side through the present-interval wrapper
 * (pw_vk_present_pe.c), which this test does not link: route it to the mock. */
NTSTATUS pw_vk_present_call(unsigned int code,void *args){return mock_call(code,args);}
void pw_vk_present_observe(unsigned int code,void *args,NTSTATUS status){(void)code;(void)args;(void)status;}
void pw_vk_present_forget(void){}
#undef pw_vk_spsc_append
static int observed_append(struct pw_vk_spsc_sequence *seq,struct pw_vk_spsc *stream,uint32_t op,const void *wire,uint32_t bytes)
{
 if(op==PW_VK_BATCH_GENERATED_OPCODE&&bytes>=4){
  unsigned code;memcpy(&code,wire,4);
  if(code==unix_vkDestroyDescriptorUpdateTemplate||code==unix_vkDestroyDescriptorUpdateTemplateKHR)
   assert(!templates.count); /* A consumer may destroy/reuse immediately upon publication. */
 }
 return pw_vk_spsc_append(seq,stream,op,wire,bytes);
}
static unsigned effects,forms;
static int effect(void *unused,const struct pw_vk_stream_record *r)
{
 unsigned code;void *params;unsigned char *arena=malloc(PW_VK_CODEC_DECODE_BYTES);const void *data=NULL;(void)unused;
 if(r->opcode==PW_VK_UPDATE_TEMPLATE){assert(pw_vk_wire_validate(r->opcode,r->payload,r->payload_bytes));assert(pw_vk_wire_u64(r->payload+32)==0x11223344&&pw_vk_wire_u64(r->payload+40)==9&&pw_vk_wire_u64(r->payload+48)==33);forms|=1;effects++;free(arena);return 0;}
 assert(r->opcode==PW_VK_BATCH_GENERATED_OPCODE);memcpy(&code,r->payload,4);
 assert(pw_vk_generated_decode(code,r->payload+4,r->payload_bytes-4,arena,PW_VK_CODEC_DECODE_BYTES,&params));
 switch(code){
 case unix_vkUpdateDescriptorSetWithTemplateKHR:data=((struct vkUpdateDescriptorSetWithTemplateKHR_params *)params)->pData;forms|=2;break;
 case unix_vkCmdPushDescriptorSetWithTemplate:data=((struct vkCmdPushDescriptorSetWithTemplate_params *)params)->pData;forms|=4;break;
 case unix_vkCmdPushDescriptorSetWithTemplateKHR:data=((struct vkCmdPushDescriptorSetWithTemplateKHR_params *)params)->pData;forms|=8;break;
 case unix_vkCmdPushDescriptorSetWithTemplate2:data=((struct vkCmdPushDescriptorSetWithTemplate2_params *)params)->pPushDescriptorSetWithTemplateInfo->pData;forms|=16;break;
 case unix_vkCmdPushDescriptorSetWithTemplate2KHR:data=((struct vkCmdPushDescriptorSetWithTemplate2KHR_params *)params)->pPushDescriptorSetWithTemplateInfo->pData;forms|=32;break;
 case unix_vkDestroyDescriptorUpdateTemplate:case unix_vkDestroyDescriptorUpdateTemplateKHR:break;
 case unix_vkCmdPipelineBarrier2:{struct vkCmdPipelineBarrier2_params *p=params;assert(p->pDependencyInfo->pMemoryBarriers[0].srcStageMask==0x12345678);break;}
 case unix_vkDestroyDevice:{struct vkDestroyDevice_params *p=params;assert(((struct vulkan_client_object *)p->device)->unix_handle==0x9876);assert(retirement.head);break;}
 default:abort();
 }
 if(data){const VkDescriptorBufferInfo *b=data;assert(b->buffer==0x11223344&&b->offset==9&&b->range==33);}
 effects++;free(arena);return 0;
}
static NTSTATUS mock_call(unsigned code,void *args)
{
 if(code==unix_vkCreateInstance){struct vkCreateInstance_params *p=args;*p->pInstance=(VkInstance)(uintptr_t)7;p->result=VK_SUCCESS;}
 if(code==unix_is_available_instance_function)return PW_VK_BATCH_CAPABILITY;
 if(code==unix_vkCreateDescriptorUpdateTemplate){struct vkCreateDescriptorUpdateTemplate_params *p=args;*p->pDescriptorUpdateTemplate=55;p->result=VK_SUCCESS;}
 if(code==unix_vkCreateDescriptorUpdateTemplateKHR){struct vkCreateDescriptorUpdateTemplateKHR_params *p=args;*p->pDescriptorUpdateTemplate=55;p->result=VK_SUCCESS;}
 if(code==unix_pw_vk_batch){struct pw_vk_batch_params *p=args;size_t done;assert(!pw_vk_stream_replay((void *)(uintptr_t)p->batch,p->bytes,effect,NULL,&done));p->status=STATUS_SUCCESS;}
 return STATUS_SUCCESS;
}
int main(int argc,char **argv)
{
 int khr=argc==2&&!strcmp(argv[1],"khr");
 VkInstance instance;VkInstanceCreateInfo instance_info={0};struct vkCreateInstance_params create={.pCreateInfo=&instance_info,.pInstance=&instance};
 VkDevice device=(VkDevice)calloc(1,sizeof(struct vulkan_client_object));struct VkCommandBuffer_T buffer={0};VkDescriptorUpdateTemplate handle;
 VkDescriptorUpdateTemplateEntry entry={.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.offset=0,.stride=sizeof(VkDescriptorBufferInfo)};
 VkDescriptorUpdateTemplateCreateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,.descriptorUpdateEntryCount=1,.pDescriptorUpdateEntries=&entry};
 VkDescriptorBufferInfo data={.buffer=0x11223344,.offset=9,.range=33};VkPushDescriptorSetWithTemplateInfo push={.sType=VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,.descriptorUpdateTemplate=55,.pData=&data};
 SetEnvironmentVariableA("PW_VK_BATCH","1");SetEnvironmentVariableA("PW_VK_BATCH_MASK",NULL);assert(!pw_vk_batch_call(unix_vkCreateInstance,&create));((struct vulkan_client_object *)device)->unix_handle=0x9876;buffer.device=device;
 if(khr){struct vkCreateDescriptorUpdateTemplateKHR_params p={.device=device,.pCreateInfo=&info,.pDescriptorUpdateTemplate=&handle};assert(!pw_vk_batch_call(unix_vkCreateDescriptorUpdateTemplateKHR,&p));}
 else {struct vkCreateDescriptorUpdateTemplate_params p={.device=device,.pCreateInfo=&info,.pDescriptorUpdateTemplate=&handle};assert(!pw_vk_batch_call(unix_vkCreateDescriptorUpdateTemplate,&p));}
 memset(&entry,0xee,sizeof(entry));assert(templates.count==1);
 {struct vkUpdateDescriptorSetWithTemplate_params p={.device=device,.descriptorSet=66,.descriptorUpdateTemplate=handle,.pData=&data};assert(!pw_vk_batch_call(unix_vkUpdateDescriptorSetWithTemplate,&p));}
 {struct vkUpdateDescriptorSetWithTemplateKHR_params p={.device=device,.descriptorSet=66,.descriptorUpdateTemplate=handle,.pData=&data};assert(!pw_vk_batch_call(unix_vkUpdateDescriptorSetWithTemplateKHR,&p));}
 {struct vkCmdPushDescriptorSetWithTemplate_params p={.commandBuffer=&buffer,.descriptorUpdateTemplate=handle,.pData=&data};assert(!pw_vk_batch_call(unix_vkCmdPushDescriptorSetWithTemplate,&p));}
 {struct vkCmdPushDescriptorSetWithTemplateKHR_params p={.commandBuffer=&buffer,.descriptorUpdateTemplate=handle,.pData=&data};assert(!pw_vk_batch_call(unix_vkCmdPushDescriptorSetWithTemplateKHR,&p));}
 {struct vkCmdPushDescriptorSetWithTemplate2_params p={.commandBuffer=&buffer,.pPushDescriptorSetWithTemplateInfo=&push};assert(!pw_vk_batch_call(unix_vkCmdPushDescriptorSetWithTemplate2,&p));}
 {struct vkCmdPushDescriptorSetWithTemplate2KHR_params p={.commandBuffer=&buffer,.pPushDescriptorSetWithTemplateInfo=&push};assert(!pw_vk_batch_call(unix_vkCmdPushDescriptorSetWithTemplate2KHR,&p));}
 memset(&data,0xee,sizeof(data));memset(&push,0xee,sizeof(push));
 if(khr){struct vkDestroyDescriptorUpdateTemplateKHR_params p={.device=device,.descriptorUpdateTemplate=handle};assert(!pw_vk_batch_call(unix_vkDestroyDescriptorUpdateTemplateKHR,&p));}
 else {struct vkDestroyDescriptorUpdateTemplate_params p={.device=device,.descriptorUpdateTemplate=handle};assert(!pw_vk_batch_call(unix_vkDestroyDescriptorUpdateTemplate,&p));}assert(!templates.count&&effects==0);
 {VkMemoryBarrier2 barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=0x12345678};VkDependencyInfo dep={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&barrier};struct vkCmdPipelineBarrier2_params p={.commandBuffer=&buffer,.pDependencyInfo=&dep};assert(!pw_vk_batch_call(unix_vkCmdPipelineBarrier2,&p));memset(&barrier,0xee,sizeof(barrier));memset(&dep,0xee,sizeof(dep));}
 {struct vkDestroyDevice_params p={.device=device};assert(!pw_vk_batch_call(unix_vkDestroyDevice,&p));}pw_vk_batch_retire_free(device);assert(retirement.head&&effects==0);
 {struct vkResetCommandBuffer_params p={0};assert(!pw_vk_batch_call(unix_vkResetCommandBuffer,&p));}assert(forms==63&&effects==9&&!retirement.head);
 puts("PASS all six template producers, overwritten inputs, metadata retirement and deferred device prefix lifetime");return 0;
}
