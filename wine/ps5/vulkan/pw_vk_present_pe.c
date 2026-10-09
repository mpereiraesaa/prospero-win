/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "vulkan_loader.h"
#include "pw_vk_present_pe.h"
#include "pw_vk_present_interval.h"
#include <string.h>
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION gate;
static LARGE_INTEGER frequency;
static BOOL enabled;
static struct {VkQueue queue;struct pw_vk_present_interval stats;} queues[8];
static BOOL CALLBACK initialize(INIT_ONCE *o,void *parameter,void **context)
{
 char value[2];(void)o;(void)parameter;(void)context;
 enabled=GetEnvironmentVariableA("PW_VK_BATCH_STATS",value,sizeof(value))==1&&value[0]=='1';
 InitializeCriticalSection(&gate);QueryPerformanceFrequency(&frequency);return TRUE;
}
void pw_vk_present_forget(void)
{
 InitOnceExecuteOnce(&once,initialize,NULL,NULL);
 if(!enabled)return;
 EnterCriticalSection(&gate);memset(queues,0,sizeof(queues));LeaveCriticalSection(&gate);
}
void pw_vk_present_observe(unsigned int code,void *args,NTSTATUS status)
{
 LARGE_INTEGER now;unsigned i;uint64_t us=0;int valid;
 struct pw_vk_present_interval copy;struct vkQueuePresentKHR_params *q=args;
 if(code!=unix_vkQueuePresentKHR&&code!=unix_vkDestroyDevice)return;
 InitOnceExecuteOnce(&once,initialize,NULL,NULL);
 if(!enabled)return;
 if(code==unix_vkDestroyDevice){if(!status)pw_vk_present_forget();return;}
 /* Capture before taking the diagnostic lock on both architectures. */
 if(!QueryPerformanceCounter(&now)){pw_vk_present_forget();return;}
 EnterCriticalSection(&gate);
 for(i=0;i<8;i++)if(!queues[i].queue||queues[i].queue==q->queue)break;
 if(i==8){LeaveCriticalSection(&gate);WINE_MESSAGE("PW_VK_PRESENT_INTERVAL version=1 dropped=1 reason=queue_capacity\n");return;}
 queues[i].queue=q->queue;
 valid=pw_vk_present_interval_add(&queues[i].stats,now.QuadPart,frequency.QuadPart,
     !status&&(q->result==VK_SUCCESS||q->result==VK_SUBOPTIMAL_KHR),&us);
 copy=queues[i].stats;LeaveCriticalSection(&gate);
 /* A failed Unix call does not promise an initialized VkResult output. */
 WINE_MESSAGE("PW_VK_PRESENT_INTERVAL version=1 scope=queue_return queue=%p tick=%llu frequency=%llu result=%d status=%lu valid=%u interval_us=%llu intervals=%llu max_us=%llu over25ms=%llu over33ms=%llu over50ms=%llu\n",
     q->queue,(UINT64)now.QuadPart,(UINT64)frequency.QuadPart,status?VK_ERROR_UNKNOWN:q->result,(ULONG)status,valid,
     (UINT64)us,(UINT64)copy.intervals,(UINT64)copy.maximum_us,(UINT64)copy.over25,(UINT64)copy.over33,(UINT64)copy.over50);
}

NTSTATUS pw_vk_present_call(unsigned int code,void *args)
{
 if(code!=unix_vkQueuePresentKHR&&code!=unix_vkDestroyDevice)return WINE_UNIX_CALL(code,args);
 InitOnceExecuteOnce(&once,initialize,NULL,NULL);
 NTSTATUS status=WINE_UNIX_CALL(code,args);
 pw_vk_present_observe(code,args,status);
 return status;
}
