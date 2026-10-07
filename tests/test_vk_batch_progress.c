/* Actual PE gate/Win32 threads, mocked synchronous Unix/driver boundary. */
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
static HANDLE wait_entered,signal_seen;
static unsigned wait_code,signal_code,trace[8],trace_count,replayed;
static DWORD wait_result;
static BOOL failing,failing_replay;
static NTSTATUS worker_status;
static struct vkQueuePresentKHR_params present_args;
static int effect(void *unused,const struct pw_vk_stream_record *r)
{
 (void)unused;assert(pw_vk_wire_validate(r->opcode,r->payload,r->payload_bytes));assert(r->opcode==PW_VK_DRAW_INDEXED);
 assert(trace_count<8);trace[trace_count++]=pw_vk_wire_u32(r->payload+4);replayed++;return 0;
}
static NTSTATUS original(unsigned code,void *args)
{
 if(code==unix_vkCreateInstance){struct vkCreateInstance_params *p=args;*p->pInstance=(VkInstance)(uintptr_t)7;p->result=VK_SUCCESS;}
 if(code==unix_is_available_instance_function)return PW_VK_BATCH_CAPABILITY;
 if(code==wait_code){
  if(failing)return STATUS_NOT_SUPPORTED;
  assert(!depth);assert(trace_count<8);trace[trace_count++]=3;
  SetEvent(wait_entered);wait_result=WaitForSingleObject(signal_seen,2000);
  assert(trace_count<8);trace[trace_count++]=5;
  if(code==unix_vkQueuePresentKHR)((struct vkQueuePresentKHR_params *)args)->result=VK_ERROR_DEVICE_LOST;
 }
 else if(code==signal_code){assert(!depth);assert(trace_count<8);trace[trace_count++]=4;SetEvent(signal_seen);}
 return STATUS_SUCCESS;
}
static NTSTATUS mock_call(unsigned code,void *args)
{
 if(code==unix_pw_vk_batch){if(failing_replay)return STATUS_UNSUCCESSFUL;struct pw_vk_batch_params *p=args;size_t done;assert(p->code==unix_count);assert(depth);assert(pw_vk_stream_replay((void *)(uintptr_t)p->batch,p->bytes,effect,NULL,&done)==PW_VK_STREAM_OK);p->status=STATUS_SUCCESS;return STATUS_SUCCESS;}
 return original(code,args);
}
static void draw(unsigned n)
{
 struct vkCmdDrawIndexed_params p={(VkCommandBuffer)(uintptr_t)9,n,1,0,-7,0};assert(!pw_vk_batch_call(unix_vkCmdDrawIndexed,&p));memset(&p,0xee,sizeof(p));
}
static DWORD WINAPI waiter(void *unused)
{
 (void)unused;worker_status=pw_vk_batch_call(wait_code,wait_code==unix_vkQueuePresentKHR?&present_args:NULL);pw_vk_batch_thread_detach();return 0;
}
static DWORD WINAPI signaler(void *unused)
{
 (void)unused;draw(2);assert(!pw_vk_batch_call(signal_code,NULL));pw_vk_batch_thread_detach();return 0;
}
int main(int argc,char **argv)
{
 static const unsigned codes[]={unix_vkWaitForFences,unix_vkWaitSemaphores,unix_vkWaitSemaphoresKHR,unix_vkQueueWaitIdle,unix_vkDeviceWaitIdle,unix_vkAcquireNextImageKHR,unix_vkAcquireNextImage2KHR,unix_vkWaitForPresentKHR,unix_vkWaitForPresent2KHR,unix_vkGetQueryPoolResults,unix_vkAcquireProfilingLockKHR,unix_vkDeferredOperationJoinKHR,unix_vkLatencySleepNV,unix_vkLatencySleepLegacyNV,unix_vkQueueSubmit,unix_vkQueueSubmit2,unix_vkQueueSubmit2KHR,unix_vkQueueBindSparse,unix_vkQueuePresentKHR,unix_vkSignalSemaphore,unix_vkSignalSemaphoreKHR};
 VkInstance handle;VkInstanceCreateInfo info={0};struct vkCreateInstance_params create={0};unsigned i,pending;HANDLE a,b;BOOL stats;
 assert(argc==2);stats=!strcmp(argv[1],"stats");SetEnvironmentVariableA("PW_VK_BATCH","1");SetEnvironmentVariableA("PW_VK_BATCH_STATS",stats?"1":"0");create.pCreateInfo=&info;create.pInstance=&handle;assert(!pw_vk_batch_call(unix_vkCreateInstance,&create));assert(enabled&&negotiated);
 if(!strcmp(argv[1],"replay-failure")){
  wait_code=unix_vkWaitSemaphores;failing_replay=TRUE;draw(1);
  pw_vk_batch_call(wait_code,NULL);assert(!"failed replay must terminate before ordinary wait");
 }
 wait_entered=CreateEventA(NULL,TRUE,FALSE,NULL);signal_seen=CreateEventA(NULL,TRUE,FALSE,NULL);assert(wait_entered&&signal_seen);
 for(i=0;i<sizeof(codes)/sizeof(codes[0]);i++)for(pending=0;pending<2;pending++){
  UINT64 before=crossings_total;unsigned count=replayed;wait_code=codes[i];signal_code=wait_code==unix_vkSignalSemaphore?unix_vkSignalSemaphoreKHR:unix_vkSignalSemaphore;trace_count=0;ResetEvent(wait_entered);ResetEvent(signal_seen);
  if(pending)draw(1);
  a=CreateThread(NULL,0,waiter,NULL,0,NULL);assert(a);assert(WaitForSingleObject(wait_entered,5000)==WAIT_OBJECT_0);
  b=CreateThread(NULL,0,signaler,NULL,0,NULL);assert(b);assert(WaitForSingleObject(a,5000)==WAIT_OBJECT_0);assert(WaitForSingleObject(b,5000)==WAIT_OBJECT_0);CloseHandle(a);CloseHandle(b);
  assert(!worker_status&&wait_result==WAIT_OBJECT_0);assert(replayed==count+1+pending);assert(trace_count==4+pending);
  assert(trace[0]==(pending?1:3));assert(trace[pending]==3&&trace[pending+1]==2&&trace[pending+2]==4&&trace[pending+3]==5);
  assert(crossings_total-before==(stats?3+pending:0));assert(!depth);
 }
 assert(present_args.result==VK_ERROR_DEVICE_LOST);assert(present==(stats?2:0));
 failing=TRUE;wait_code=unix_vkWaitForFences;draw(1);assert(pw_vk_batch_call(wait_code,NULL)==STATUS_NOT_SUPPORTED);assert(!depth);failing=FALSE;
 assert(!pw_vk_stream_progress_call(unix_vkDestroyDevice));assert(!pw_vk_stream_progress_call(unix_vkResetCommandBuffer));assert(!pw_vk_stream_progress_call(unix_vkCmdWaitEvents));
 printf("PASS 21 progress APIs pending/empty replay, concurrent signal and second replay, owned bytes, status/device-loss preservation, stats=%u\n",stats);return 0;
}
