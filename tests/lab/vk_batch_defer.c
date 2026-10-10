/* Deferred descriptor writes: the actual PE adapter and Win32 APIs, only the
 * Unix crossing is mocked. The mock driver logs its effects in order. */
#include <assert.h>
#include <stdio.h>
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
/* raw_call reaches the Unix side through the present-interval wrapper
 * (pw_vk_present_pe.c), which this test does not link: route it to the mock. */
NTSTATUS pw_vk_present_call(unsigned int code,void *args){return mock_call(code,args);}
void pw_vk_present_observe(unsigned int code,void *args,NTSTATUS status){(void)code;(void)args;(void)status;}
void pw_vk_present_forget(void){}

#define DEVICE ((VkDevice)(uintptr_t)5)
#define MEMORY ((VkDeviceMemory)UINT64_C(0x1000))
/* The allocation is MAPPED_SIZE; the slack takes a write past its end. */
#define MAPPED_SIZE 4096
static DECLSPEC_ALIGN(64) unsigned char mapped[MAPPED_SIZE+64];
static unsigned char heap_out[64];
static char effects[64][16];
static unsigned effect_count,sync_writes,deferred_writes;
static void note(const char *what){assert(effect_count<64);snprintf(effects[effect_count++],sizeof(effects[0]),"%s",what);}
/* The mock driver's descriptor: the buffer address's low byte, repeated. */
static void write_descriptor(const struct vkGetDescriptorEXT_params *p)
{
 assert(p->pDescriptorInfo->type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
 memset(p->pDescriptor,(int)(p->pDescriptorInfo->data.pUniformBuffer->address&0xff),p->dataSize);
}
static int effect(void *unused,const struct pw_vk_stream_record *r)
{
 static union{uint64_t align;unsigned char bytes[1<<16];} arena;
 unsigned code;void *params;(void)unused;
 assert(r->opcode==PW_VK_BATCH_GENERATED_OPCODE);
 memcpy(&code,r->payload,4);
 assert(pw_vk_generated_decode(code,r->payload+4,r->payload_bytes-4,arena.bytes,sizeof(arena.bytes),&params));
 if(code==unix_vkGetDescriptorEXT){write_descriptor(params);deferred_writes++;note("deferred");}
 else if(code==unix_vkUnmapMemory)note("unmap");
 else if(code==unix_vkFreeMemory)note("free");
 else assert(!"unexpected record");
 return 0;
}
static NTSTATUS original(unsigned int code,void *args)
{
 switch(code){
 case unix_vkCreateInstance:{struct vkCreateInstance_params *p=args;*p->pInstance=(VkInstance)(uintptr_t)7;p->result=VK_SUCCESS;break;}
 case unix_is_available_instance_function:{struct is_available_instance_function_params *p=args;return !strcmp(p->name,PW_VK_BATCH_NAME)?PW_VK_BATCH_CAPABILITY:0;}
 case unix_vkAllocateMemory:{struct vkAllocateMemory_params *p=args;*p->pMemory=MEMORY;p->result=VK_SUCCESS;break;}
 case unix_vkMapMemory:{struct vkMapMemory_params *p=args;*p->ppData=mapped+p->offset;p->result=VK_SUCCESS;break;}
 case unix_vkUnmapMemory:note("unmap");break;
 case unix_vkFreeMemory:note("free");break;
 case unix_vkQueueSubmit:{struct vkQueueSubmit_params *p=args;p->result=VK_SUCCESS;note("submit");break;}
 case unix_vkGetDescriptorEXT:write_descriptor(args);sync_writes++;note("sync");break;
 default:break;
 }
 return STATUS_SUCCESS;
}
static NTSTATUS mock_call(unsigned int code,void *args)
{
 if(code==unix_pw_vk_batch){
  struct pw_vk_batch_params *p=args;size_t done;
  assert(p->version==PW_VK_BATCH_VERSION);
  assert(pw_vk_stream_replay((void *)(uintptr_t)p->batch,p->bytes,effect,NULL,&done)==PW_VK_STREAM_OK);
  p->status=p->code>=unix_count?STATUS_SUCCESS:original(p->code,(void *)(uintptr_t)p->args);
  return STATUS_SUCCESS;
 }
 return original(code,args);
}
static void get(void *destination,size_t size,uint64_t address)
{
 VkDescriptorAddressInfoEXT info={VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,NULL,address,256,VK_FORMAT_UNDEFINED};
 VkDescriptorGetInfoEXT get_info={VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,NULL,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,{0}};
 struct vkGetDescriptorEXT_params p={DEVICE,&get_info,size,destination};
 get_info.data.pUniformBuffer=&info;
 assert(pw_vk_batch_call(unix_vkGetDescriptorEXT,&p)==STATUS_SUCCESS);
 /* The record owns its input: the caller's structures may change at once. */
 memset(&info,0xee,sizeof(info));memset(&get_info,0xee,sizeof(get_info));
}
static void map(VkDeviceSize offset,VkDeviceSize size)
{
 void *data=NULL;struct vkMapMemory_params p={DEVICE,MEMORY,offset,size,0,&data,VK_ERROR_UNKNOWN};
 assert(pw_vk_batch_call(unix_vkMapMemory,&p)==STATUS_SUCCESS&&p.result==VK_SUCCESS&&data==mapped+offset);
}
static void unmap(void){struct vkUnmapMemory_params p={DEVICE,MEMORY};assert(pw_vk_batch_call(unix_vkUnmapMemory,&p)==STATUS_SUCCESS);}
static void submit(void){struct vkQueueSubmit_params p={0};assert(pw_vk_batch_call(unix_vkQueueSubmit,&p)==STATUS_SUCCESS);}
static void expect(const char *const *order,unsigned n)
{
 unsigned i;
 if(effect_count!=n)fprintf(stderr,"effects %u, want %u\n",effect_count,n);
 assert(effect_count==n);
 for(i=0;i<n;i++){if(strcmp(effects[i],order[i]))fprintf(stderr,"effect %u: %s, want %s\n",i,effects[i],order[i]);assert(!strcmp(effects[i],order[i]));}
}
int main(int argc,char **argv)
{
 VkInstance instance=NULL;VkInstanceCreateInfo create={0};
 struct vkCreateInstance_params instance_params={0};
 VkMemoryAllocateInfo allocate={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,MAPPED_SIZE,0};
 VkDeviceMemory memory=0;struct vkAllocateMemory_params allocate_params={DEVICE,&allocate,NULL,&memory,VK_ERROR_UNKNOWN};
 BOOL on;
 create.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
 instance_params.pCreateInfo=&create;instance_params.pInstance=&instance;instance_params.result=VK_ERROR_UNKNOWN;
 assert(argc==2);on=!strcmp(argv[1],"on");assert(on||!strcmp(argv[1],"off"));
 SetEnvironmentVariableA("PW_VK_BATCH","1");SetEnvironmentVariableA("PW_VK_BATCH_STATS",NULL);
 SetEnvironmentVariableA("PW_VK_DEFER_DESCRIPTORS",on?"1":NULL);
 assert(pw_vk_batch_call(unix_vkCreateInstance,&instance_params)==STATUS_SUCCESS&&negotiated);
 assert(pw_vk_batch_call(unix_vkAllocateMemory,&allocate_params)==STATUS_SUCCESS&&memory==MEMORY);
 assert(defer_descriptors==on);
 map(0,VK_WHOLE_SIZE);

 /* Into the whole-size mapping: deferred when on, written at once when off. */
 get(mapped+64,16,0x11);
 assert(mapped[64]==(on?0:0x11)&&sync_writes==(on?0u:1u));
 /* A heap destination, and one past the mapping's end, stay synchronous. */
 get(heap_out,16,0x22);
 assert(heap_out[0]==0x22&&heap_out[15]==0x22);
 get(mapped+MAPPED_SIZE-8,16,0x23);
 assert(sync_writes==(on?2u:3u));
 /* A submit runs after the deferred write it may depend on. */
 submit();
 assert(mapped[64]==0x11&&mapped[79]==0x11&&mapped[80]==0);
 /* A synchronous call flushes what was enqueued before it first. */
 if(on){static const char *const order[]={"deferred","sync","sync","submit"};expect(order,4);}
 else{static const char *const order[]={"sync","sync","sync","submit"};expect(order,4);}
 effect_count=0;

 /* A write deferred before an unmap lands before it; after it, synchronous. */
 get(mapped+128,16,0x33);
 unmap();
 get(mapped+256,16,0x44);
 assert(mapped[128]==0x33&&mapped[256]==0x44);
 if(on){static const char *const order[]={"deferred","unmap","sync"};expect(order,3);}
 else{static const char *const order[]={"sync","unmap","sync"};expect(order,3);}
 effect_count=0;

 /* An explicit size bounds the mapping: inside defers, outside does not. */
 map(0,1024);
 get(mapped+512,16,0x55);
 get(mapped+2048,16,0x66);
 submit();
 assert(mapped[512]==0x55&&mapped[2048]==0x66);
 if(on){static const char *const order[]={"deferred","sync","submit"};expect(order,3);assert(deferred_writes==3);}
 else{static const char *const order[]={"sync","sync","submit"};expect(order,3);assert(deferred_writes==0);}
 printf("PASS deferred descriptor writes %s: %u deferred, %u synchronous\n",argv[1],deferred_writes,sync_writes);
 return 0;
}
