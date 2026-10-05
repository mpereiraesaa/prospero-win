/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Minimal PS5 AGC DMA presenter.  The packet ordering follows the order
 * measured on the console on FW 12.02:
 * wait-safe -> memory DMA (L2, synchronized) -> SetFlip -> RELEASE_MEM fence. */
#include "pw_agc_ps5.h"
#include "pw_agc_submit_lifecycle.h"
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { AGC_MODULE=0x80000094u,COMMAND_BYTES=0x20000u,COMMAND_ALIGNMENT=0x10000u,
       COMMAND_SLOT_DWORDS=0x1000u/4u,FENCE_OFFSET=0x1100u };
typedef struct AgcWriter {
    uint32_t *bottom,*top,*up,*down;
    uintptr_t callback;
    void *user_data;
    uint32_t reserved_dwords,padding;
} AgcWriter;
typedef struct BatchMapEntry {
    void *address;int64_t physical;size_t length;
    uint8_t protection,memory_type;uint16_t reserved;uint32_t operation;
} BatchMapEntry;
typedef struct AgcSubmit {
    const uint32_t *words;uint32_t count;uint8_t flag,padding[3];
} AgcSubmit;
_Static_assert(sizeof(AgcWriter)==0x38,"AGC writer ABI");
_Static_assert(sizeof(BatchMapEntry)==0x20,"BatchMap ABI");
_Static_assert(sizeof(AgcSubmit)==0x10,"AGC submit ABI");

extern int sceSysmoduleLoadModuleInternal(unsigned int,...);
extern int sceSysmoduleUnloadModuleInternal(unsigned int,...);
extern int sceKernelReserveVirtualRange(void **,size_t,int,size_t);
extern int sceKernelAllocateMainDirectMemory(size_t,size_t,int,int64_t *);
extern int sceKernelBatchMap(void *,int,int *);
extern int sceKernelMunmap(void *,size_t);
extern int sceKernelReleaseDirectMemory(int64_t,size_t);
extern int32_t sceAgcInit(void *,uint32_t);
extern uint32_t *sceAgcDcbDmaData(void *,uint32_t,uint32_t,uint32_t,uint64_t,
                                  uint32_t,uint32_t,uint64_t,uint32_t,uint32_t,
                                  uint32_t,uint32_t);
extern uint32_t *sceAgcDcbSetFlip(void *,uint32_t,int32_t,uint32_t,int64_t);
extern int32_t sceAgcDriverSubmitDcb(void *);
extern int32_t sceAgcSuspendPoint(void);
extern uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);
extern uint32_t sceAgcDriverWaitUntilSafeForRendering(uint32_t **,uint32_t,
                                                      uint32_t,uint32_t,int32_t);

static uint8_t no_space(AgcWriter *writer,uint32_t requested,void *opaque)
{(void)writer;(void)requested;(void)opaque;return 0;}
static void writer_begin(AgcWriter *writer,uint32_t *cursor,uint32_t *end)
{
    *writer=(AgcWriter){.bottom=cursor,.top=end,.up=cursor,.down=end,
        .callback=(uintptr_t)no_space};
}
static int writer_finish(const AgcWriter *writer,uint32_t *begin,uint32_t *end,
                         uint32_t **cursor)
{
    if(writer->bottom!=begin || writer->top!=end || writer->down!=end ||
       writer->up<begin || writer->up>end)return PW_ERR_STATE;
    *cursor=writer->up;return PW_OK;
}
static void flush(const void *address,size_t bytes)
{
    const uint8_t *line=address,*end=line+bytes;
    for(;line<end;line+=64)__asm__ volatile("clflush (%0)"::"r"(line):"memory");
    __asm__ volatile("mfence":::"memory");
}
static uint64_t monotonic_ns(void)
{
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
    return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
}
int pw_agc_ps5_open(PwAgcPs5 *agc)
{
    if(!agc)return PW_ERR_PRECONDITION;memset(agc,0,sizeof(*agc));agc->physical=-1;
    int result=sceSysmoduleLoadModuleInternal(AGC_MODULE);if(result)return PW_ERR_STATE;
    agc->module_loaded=1;uint64_t state=0;
    if(sceAgcInit(&state,sizeof(state)))goto state_failed;
    if(sceKernelReserveVirtualRange(&agc->command,COMMAND_BYTES,0,COMMAND_ALIGNMENT) ||
       !agc->command)goto vm_failed;
    agc->reserved=1;
    if(sceKernelAllocateMainDirectMemory(COMMAND_BYTES,COMMAND_ALIGNMENT,0x0c,
                                         &agc->physical))goto vm_failed;
    agc->allocated=1;
    BatchMapEntry entry={agc->command,agc->physical,COMMAND_BYTES,0xf2,0x0c,0,0};
    int processed=-1;
    if(sceKernelBatchMap(&entry,1,&processed) || processed!=1)goto vm_failed;
    agc->mapped=1;agc->bytes=COMMAND_BYTES;
    memset(agc->command,0,COMMAND_BYTES);
    agc->fence=(volatile uint64_t *)((uint8_t *)agc->command+FENCE_OFFSET);
    return PW_OK;
vm_failed:
    (void)pw_agc_ps5_close(agc);return PW_ERR_VM;
state_failed:
    (void)pw_agc_ps5_close(agc);return PW_ERR_STATE;
}
int pw_agc_ps5_close(PwAgcPs5 *agc)
{
    if(!agc)return PW_ERR_PRECONDITION;
    int status=PW_OK;agc->unmap_rc=agc->release_rc=agc->munmap_rc=agc->unload_rc=0;
    if(agc->mapped) {
        BatchMapEntry entry={agc->command,0,COMMAND_BYTES,0xf2,0x0c,0,1};
        int processed=-1;agc->unmap_rc=sceKernelBatchMap(&entry,1,&processed);
        if(agc->unmap_rc || processed!=1)status=PW_ERR_STATE;
        else agc->mapped=0;
    }
    if(agc->allocated) {
        agc->release_rc=sceKernelReleaseDirectMemory(agc->physical,COMMAND_BYTES);
        if(agc->release_rc)status=PW_ERR_STATE;else agc->allocated=0;
    }
    if(agc->reserved) {
        agc->munmap_rc=sceKernelMunmap(agc->command,COMMAND_BYTES);
        if(agc->munmap_rc)status=PW_ERR_STATE;else agc->reserved=0;
    }
    if(agc->module_loaded) {
        agc->unload_rc=sceSysmoduleUnloadModuleInternal(AGC_MODULE);
        if(agc->unload_rc)status=PW_ERR_STATE;else agc->module_loaded=0;
    }
    agc->command=NULL;agc->fence=NULL;agc->physical=-1;agc->bytes=0;
    return status;
}
int pw_agc_ps5_copy_flip(PwAgcPs5 *agc,int video_handle,int buffer_index,
                         const void *source,void *destination,uint32_t bytes,
                         uint64_t flip_arg)
{
    if(!agc || !agc->mapped || video_handle<0 || buffer_index<0 || buffer_index>1 ||
       !source || !destination || !bytes || bytes>0x03ffffffu)return PW_ERR_PRECONDITION;
    uint32_t *begin=agc->command,*end=begin+COMMAND_SLOT_DWORDS,*cursor=begin;
    memset(begin,0,0x1000);*agc->fence=1;
    uint32_t wait_words=sceAgcDriverGetWaitRenderingPacketSizeInDwords();
    if(!wait_words || wait_words>(uint32_t)(end-cursor) ||
       sceAgcDriverWaitUntilSafeForRendering(&cursor,wait_words,0,
                                             (uint32_t)video_handle,buffer_index) ||
       cursor!=begin+wait_words)return PW_ERR_STATE;
    AgcWriter writer;uint32_t *packet=cursor;writer_begin(&writer,packet,end);
    /* GFX1013's address-through-L2 source form is selector 3.  raw_wait is a
     * source-hazard policy, not a generic ordering bit; PAL and our validated
     * FW 12.02 packet contract leave it clear and use cp_sync for completion. */
    (void)sceAgcDcbDmaData(&writer,0,3,0,(uint64_t)(uintptr_t)destination,
                           3,0,(uint64_t)(uintptr_t)source,bytes,0,0,1);
    if(writer_finish(&writer,packet,end,&cursor)!=PW_OK || cursor<=packet)
        return PW_ERR_STATE;
    packet=cursor;writer_begin(&writer,packet,end);
    (void)sceAgcDcbSetFlip(&writer,(uint32_t)video_handle,buffer_index,1,(int64_t)flip_arg);
    if(writer_finish(&writer,packet,end,&cursor)!=PW_OK || cursor<=packet || end-cursor<8)
        return PW_ERR_STATE;
    const uintptr_t fence=(uintptr_t)agc->fence;
    const uint32_t release[8]={0xc0064900u,0x06000528u,0x42010000u,
        (uint32_t)fence,(uint32_t)(fence>>32),0,0,0};
    memcpy(cursor,release,sizeof(release));cursor+=8;
    flush(source,bytes);flush(begin,(size_t)(cursor-begin)*4u);flush((const void *)agc->fence,8);
    AgcSubmit submit={begin,(uint32_t)(cursor-begin),0,{0,0,0}};
    /* Close Game may suspend the process immediately after a frame.  A fence
     * proves completion, but only sceAgcSuspendPoint makes the submitted AGC
     * queue eligible for the system suspension lifecycle. */
    int submit_status=pw_agc_submit_and_suspend(&submit,sceAgcDriverSubmitDcb,
                                                sceAgcSuspendPoint);
    if(submit_status!=PW_OK)return submit_status;
    const uint64_t deadline=monotonic_ns()+2000000000ull;
    while(__atomic_load_n(agc->fence,__ATOMIC_ACQUIRE)!=0 && monotonic_ns()<deadline)
        (void)usleep(1000);
    if(__atomic_load_n(agc->fence,__ATOMIC_ACQUIRE)!=0)return PW_ERR_LIMIT;
    agc->submits++;return PW_OK;
}
