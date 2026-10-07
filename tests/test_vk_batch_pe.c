/* Actual Win32 APIs and real Wine structs; only Unix crossing is mocked. */
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
static unsigned ordinary,batches,replayed,draw_raw,sequence[4096],used;
static BOOL support=TRUE;
static int effect(void *unused,const struct pw_vk_stream_record *r){(void)unused;assert(pw_vk_wire_validate(r->opcode,r->payload,r->payload_bytes));assert(r->opcode==PW_VK_DRAW_INDEXED);assert(used<4096);sequence[used++]=pw_vk_wire_u32(r->payload+4);replayed++;return 0;}
static NTSTATUS original(unsigned int code,void *args)
{
 ordinary++;
 if(code==unix_vkCreateInstance){struct vkCreateInstance_params *p=args;*p->pInstance=(VkInstance)(uintptr_t)7;p->result=VK_SUCCESS;}
 if(code==unix_is_available_instance_function){struct is_available_instance_function_params *p=args;if(!strcmp(p->name,PW_VK_BATCH_NAME))return support?PW_VK_BATCH_CAPABILITY:0;}
 if(code==unix_vkCmdDrawIndexed)draw_raw++;
 if(code==unix_vkCreateDebugUtilsMessengerEXT){struct is_available_instance_function_params p={(VkInstance)(uintptr_t)7,"vkDummy"};assert(pw_vk_batch_call(unix_is_available_instance_function,&p)==STATUS_SUCCESS);}
 return STATUS_SUCCESS;
}
static NTSTATUS mock_call(unsigned int code,void *args)
{
 if(code==unix_pw_vk_batch){struct pw_vk_batch_params *p=args;size_t done;batches++;assert(support);assert(p->version==1);assert(pw_vk_stream_replay((void *)(uintptr_t)p->batch,p->bytes,effect,NULL,&done)==PW_VK_STREAM_OK);p->status=p->code==unix_count?STATUS_SUCCESS:original(p->code,(void *)(uintptr_t)p->args);return STATUS_SUCCESS;}
 return original(code,args);
}
static void draw_index(unsigned n){struct vkCmdDrawIndexed_params p={(VkCommandBuffer)(uintptr_t)9,n,1,0,-7,0};assert(pw_vk_batch_call(unix_vkCmdDrawIndexed,&p)==STATUS_SUCCESS);memset(&p,0xee,sizeof(p));}
static DWORD WINAPI worker(void *unused){(void)unused;draw_index(11);draw_index(12);pw_vk_batch_thread_detach();return 0;}
int main(int argc,char **argv)
{
 VkInstance handle;VkInstanceCreateInfo info={0};struct vkCreateInstance_params create={0};HANDLE thread;struct vkDestroyDevice_params destroy={0};unsigned prior;
 assert(argc==2);support=strcmp(argv[1],"old")!=0;SetEnvironmentVariableA("PW_VK_BATCH",(!strcmp(argv[1],"off")||!strcmp(argv[1],"stats"))?"0":"1");SetEnvironmentVariableA("PW_VK_BATCH_STATS",(!strcmp(argv[1],"stats")||!strcmp(argv[1],"profile"))?"1":"0");SetEnvironmentVariableA("PW_VK_BATCH_FALLBACK_PROFILE",(!strcmp(argv[1],"profile")||!strcmp(argv[1],"profile-no-stats"))?"1":"0");
 create.pCreateInfo=&info;create.pInstance=&handle;assert(pw_vk_batch_call(unix_vkCreateInstance,&create)==0);
 if(!strcmp(argv[1],"off")||!strcmp(argv[1],"stats")||!support){draw_index(1);assert(draw_raw==1&&batches==0);if(!strcmp(argv[1],"stats")){struct vkQueuePresentKHR_params q={0};assert(pw_vk_batch_call(unix_vkQueuePresentKHR,&q)==0);assert(crossings_total==3&&present==1);}else assert(crossings_total==0);puts("PASS original path and old-Unix capability without new table access");return 0;}
 assert(negotiated&&enabled);
 if(!strcmp(argv[1],"profile")){
  unsigned i;struct vkQueuePresentKHR_params q={0};
  struct vkCmdSetViewport_params viewport={0};struct vkCmdPipelineBarrier2_params barrier={0};
  for(i=0;i<300;i++){
   unsigned j;for(j=0;j<3;j++)assert(pw_vk_batch_call(unix_vkCmdSetViewport,&viewport)==0);
   for(j=0;j<4;j++)assert(pw_vk_batch_call(unix_vkCmdPipelineBarrier2,&barrier)==0);
   draw_index(1);assert(pw_vk_batch_call(unix_vkQueuePresentKHR,&q)==0);
  }
  assert(fallback_profile&&fallback_total==2401&&fallback_report_present==300);
  assert(fallback_counts[unix_vkCmdPipelineBarrier2]==1200&&fallback_counts[unix_vkCmdSetViewport]==900);
  assert(enqueued_total==300&&records_total==300);
  for(i=0;i<300;i++)assert(pw_vk_batch_call(unix_vkQueuePresentKHR,&q)==0);
  assert(fallback_report_present==600&&fallback_total==2701);
  /* Exercise bounded top-8 selection and deterministic ascending-code ties
   * independently of the Unix fixture's operation-specific behavior. */
  for(i=0;i<10;i++)fallback_counts[i]+=100;
  fallback_snapshot(900);
  assert(fallback_report_present==900);
  puts("PASS fallback profile exact counts, period deltas, top-8 and ties");return 0;
 }
 assert(!fallback_profile);thread=CreateThread(NULL,0,worker,NULL,0,NULL);assert(thread);assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);assert(replayed==0);
 /* A producer's owned arena survives actual Win32 thread exit. Main's destroy
  * drains both records before the original lifetime operation. */
 assert(pw_vk_batch_call(unix_vkDestroyDevice,&destroy)==0);assert(batches==1&&replayed==2&&sequence[0]==11&&sequence[1]==12);assert(registry.streams==NULL);
 draw_index(13);prior=ordinary;
 {struct vkCreateDebugUtilsMessengerEXT_params callback={0};assert(pw_vk_batch_call(unix_vkCreateDebugUtilsMessengerEXT,&callback)==0);}
 assert(sticky_disabled&&replayed==3&&sequence[2]==13&&ordinary==prior+2);
 draw_index(14);assert(draw_raw==1);assert(!depth);
 assert(crossings_total==0);puts("PASS actual Win32 TLS retirement helper, cross-thread lifetime drain, piggyback and sticky callback reentry; automatic DLL notification is source-checked separately");return 0;
}
