/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_kinds.h"
#ifdef PW_D3D9_ENABLE_QUERY
#include "pw_d3d9_service_query.h"
#endif
#ifdef PW_D3D9_ENABLE_UP
#include "pw_d3d9_service_up.h"
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
#include "pw_d3d9_service_object_getter.h"
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
#include "pw_d3d9_service_texture.h"
#endif
#include "pw_d3d9_session.h"
#include "pw_d3d9_failure_wait.h"
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
#include "pw_d3d9_native_draw_state.h"
#include "pw_d3d9_transform_observer.h"
#endif
#ifdef PW_D3D9_ENABLE_API_OBSERVE
#include "pw_d3d9_api_observe.h"
#endif
#include "pw_d3d9_failure_diag.h"
#ifdef PW_D3D9_ENABLE_STATEBLOCK
#include "pw_d3d9_service_stateblock.h"
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
#include "pw_d3d9_service_program.h"
#endif
#ifdef PW_D3D9_ENABLE_METHODS
#include "pw_d3d9_service_methods.h"
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
#include "pw_d3d9_service_resource.h"
#endif
#include "../pw_d3d9_bridge_wire.h"
#include "../pw_d3d9_transport_stats.h"
#ifdef PW_D3D9_ENABLE_BATCH
#include "../pw_d3d9_command_batch.h"
#include "../pw_d3d9_command_policy.h"
#endif
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
#include "../pw_d3d9_binding_plan.h"
#endif
#include <d3d9.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#define SESSION_MAGIC 0x39535750u
#define RING_BYTES 8192u
#define BACKEND_BYTES 3919872u
#define BACKEND_CRC 0x6d86db72u
#define FACTORY_METHODS 0x00007ff0u /* slots 4 through 14 */
#define TRANSPORT_ERROR 0x100u
/* Bounded receive spin before blocking on the wake event. Back-to-back D3D9
 * calls usually answer within a few microseconds, below one wineserver
 * wait/signal round trip; a peer that stays idle costs at most this spin.
 * 512 pause-polls measured 28-48 us on an Alder Lake host (long PAUSE); Zen 2
 * PAUSE is shorter, so the console budget should be lower (not yet measured). */
#define PW_D3D9_SPIN_POLLS 512u
/* This mask is part of the checked HELLO payload: reject differently built
 * proxy/service pairs before any object publication or method dispatch. */
static uint32_t compiled_features(void)
{
    uint32_t mask=0;
#ifdef PW_D3D9_ENABLE_DEVICE
    mask|=1u;
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
    mask|=2u;
#endif
#ifdef PW_D3D9_ENABLE_METHODS
    mask|=4u;
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
    mask|=8u;
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
    mask|=16u;
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
    mask|=32u;
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
    mask|=64u;
#endif
#ifdef PW_D3D9_ENABLE_UP
    mask|=128u;
#endif
#ifdef PW_D3D9_ENABLE_QUERY
    mask|=256u;
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
    mask|=512u;
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
    mask|=1024u;
#endif
#ifdef PW_D3D9_ENABLE_IMPLICIT
    mask|=4096u;
#endif
#ifdef PW_D3D9_ENABLE_BATCH
    mask|=8192u | PW_D3D9_COMMAND_POLICY_FEATURE;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    mask|=PW_D3D9_BINDING_FEATURE;
#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
    mask|=PW_D3D9_DRAW_FEATURE|PW_D3D9_TRANSFORM_FEATURE;
#endif
#endif
    return mask;
}
struct descriptor {
    uint32_t magic,version,bytes,epoch,pid,ring_bytes,backend_crc,backend_bytes;
    WCHAR backend[260];
};
_Static_assert(sizeof(struct descriptor)==552,"descriptor width");
struct ipc {
    HANDLE descriptor_mapping,wire_mapping,request,reply,opened,cancel;
    struct descriptor *descriptor;
    void *memory;
    struct pw_d3d9_channel channel;
    SRWLOCK stats_lock;
    struct pw_d3d9_transport_stats stats,interval;
    uint64_t presents;
};
/* PW_D3D9_PROFILE is independent of verbose API diagnostics. No clocks when off. */
static INIT_ONCE profile_once=INIT_ONCE_STATIC_INIT;
static int profile_on;
static uint64_t profile_frequency;
static BOOL CALLBACK profile_init(INIT_ONCE *once,void *parameter,void **context)
{
    char value[8];LARGE_INTEGER frequency;(void)once;(void)parameter;(void)context;
    profile_on=GetEnvironmentVariableA("PW_D3D9_PROFILE",value,sizeof(value))==1&&value[0]=='1';
    if(profile_on&&QueryPerformanceFrequency(&frequency)&&frequency.QuadPart>0)profile_frequency=(uint64_t)frequency.QuadPart;
    return TRUE;
}
static int profile_enabled(void)
{DWORD error=GetLastError();int saved=errno;InitOnceExecuteOnce(&profile_once,profile_init,NULL,NULL);
 SetLastError(error);errno=saved;return profile_on;}
static uint64_t profile_now(void)
{
    DWORD error=GetLastError();int saved=errno;LARGE_INTEGER t;uint64_t result=0;
    if(profile_frequency&&QueryPerformanceCounter(&t)&&t.QuadPart>0)result=pw_d3d9_stats_ticks_us((uint64_t)t.QuadPart,profile_frequency);
    SetLastError(error);errno=saved;return result;
}
struct profile_record {
    struct pw_d3d9_transport_stats total,delta;
    uint64_t frame,sequence;
    uint32_t epoch,object,generation,status;
    int emit,reply_valid,final,startup,clock_valid;
};
static void profile_capture(struct ipc *i,const struct pw_d3d9_transport_stats *sample,
                            const struct pw_d3d9_message *m,uint64_t sequence,HRESULT hr,
                            int present,int reply_valid,int final,struct profile_record *out)
{
    out->emit=0;if(!profile_enabled())return;
    AcquireSRWLockExclusive(&i->stats_lock);
    pw_d3d9_stats_add(&i->stats,sample);pw_d3d9_stats_add(&i->interval,sample);
    if(present||final){
        out->startup=i->presents==0;
        if(present)i->presents++;
        out->total=i->stats;out->delta=i->interval;memset(&i->interval,0,sizeof(i->interval));
        out->frame=i->presents;out->sequence=sequence;out->epoch=i->channel.epoch;
        out->object=m?m->object:0;out->generation=m?m->generation:0;out->status=(uint32_t)hr;
        out->reply_valid=reply_valid;out->final=final;out->clock_valid=profile_frequency!=0&&!out->delta.clock_invalid;out->emit=1;
    }
    ReleaseSRWLockExclusive(&i->stats_lock);
}
static void profile_emit(const struct profile_record *r)
{
    if(!r->emit)return;
    DWORD error=GetLastError();int saved=errno;
    const struct pw_d3d9_transport_stats *d=&r->delta,*t=&r->total;
    char line[4096];
    int used=snprintf(line,sizeof(line),"PW_D3D9_PROFILE transport role=%s pid=%lu tid=%lu domain=%s scope=session_interval epoch=%u frame=%llu seq=%llu object=%u generation=%u hr=%08x reply_valid=%d final=%d startup=%d clock_valid=%d saturated=%llu attempts=%llu sync_published=%llu replies=%llu failures=%llu rejected_present=%llu async_queued=%llu batch_flushes=%llu batch_commands=%llu batch_residence_wall_us=%llu request_bytes=%llu reply_bytes=%llu serial_wait_wall_us=%llu guest_wait_wall_us=%llu roundtrip_wall_us=%llu service_dispatch_wall_us=%llu total_attempts=%llu total_sync_published=%llu total_replies=%llu",
#ifdef _WIN64
        "service",GetCurrentProcessId(),GetCurrentThreadId(),"native64",
#else
        "client",GetCurrentProcessId(),GetCurrentThreadId(),"guest32",
#endif
        r->epoch,(unsigned long long)r->frame,(unsigned long long)r->sequence,r->object,r->generation,r->status,r->reply_valid,r->final,r->startup,
        r->clock_valid,(unsigned long long)t->saturated,(unsigned long long)d->attempts,(unsigned long long)d->published,(unsigned long long)d->replies,(unsigned long long)d->failures,(unsigned long long)d->rejected_present,
        (unsigned long long)d->async_queued,(unsigned long long)d->batch_flushes,(unsigned long long)d->batch_commands,(unsigned long long)d->batch_residence_wall_us,
        (unsigned long long)d->request_bytes,(unsigned long long)d->reply_bytes,(unsigned long long)d->serial_wait_wall_us,(unsigned long long)d->guest_wait_wall_us,(unsigned long long)d->roundtrip_wall_us,(unsigned long long)d->service_dispatch_wall_us,
        (unsigned long long)t->attempts,(unsigned long long)t->published,(unsigned long long)t->replies);
    for(unsigned n=0;n<PW_D3D9_STATS_OPS&&used>0&&(size_t)used<sizeof(line)-64;n++)
        if(d->opcode[n])used+=snprintf(line+used,sizeof(line)-(size_t)used," op%u=%llu",n,(unsigned long long)d->opcode[n]);
    if(used>0)fprintf(stderr,"%s\n",line);
    SetLastError(error);errno=saved;
}
static int profile_present(const struct pw_d3d9_message *m,const void *payload)
{
    const unsigned char *p=payload;
    return m->opcode==PW_D3D9_DEVICE_CALL&&m->payload_bytes>=8&&p&&p[4]==3&&!p[5]&&!p[6]&&!p[7];
}
static void put32(unsigned char *p,uint32_t n)
{ for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(i*8)); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int absolute_path(const WCHAR *p)
{ return p && p[0] && p[1]==':' && p[2]=='\\' && wcsnlen(p,260)<260; }
static void name(WCHAR *out,const WCHAR *kind,DWORD pid,DWORD epoch)
{ swprintf(out,96,L"Local\\PW_D3D9_%ls_%08lx_%08lx",kind,pid,epoch); }
static void close_ipc(struct ipc *i)
{
    if(profile_enabled()){struct profile_record record;struct pw_d3d9_transport_stats empty={0};
        profile_capture(i,&empty,NULL,0,E_FAIL,0,0,1,&record);profile_emit(&record);}
    if(i->memory)UnmapViewOfFile(i->memory);
    if(i->descriptor)UnmapViewOfFile(i->descriptor);
    HANDLE handles[]={i->request,i->reply,i->opened,i->cancel,i->wire_mapping,i->descriptor_mapping};
    for(unsigned n=0;n<sizeof(handles)/sizeof(handles[0]);n++)if(handles[n])CloseHandle(handles[n]);
    memset(i,0,sizeof(*i));
}
static void cancel_ipc(struct ipc *i)
{
    if(i->channel.memory)pw_d3d9_channel_cancel(&i->channel,TRANSPORT_ERROR);
    if(i->cancel)SetEvent(i->cancel);
    if(i->request)SetEvent(i->request);
    if(i->reply)SetEvent(i->reply);
}
static void hello_payload(unsigned char bytes[32],DWORD epoch)
{
    uint32_t fields[]={1,epoch,RING_BYTES,BACKEND_CRC,BACKEND_BYTES,FACTORY_METHODS,compiled_features(),0};
    for(unsigned n=0;n<8;n++)put32(bytes+n*4,fields[n]);
}
#ifndef _WIN64
static INIT_ONCE tls_once=INIT_ONCE_STATIC_INIT;
static DWORD callback_tls=TLS_OUT_OF_INDEXES,quit_seen_tls=TLS_OUT_OF_INDEXES,quit_code_tls=TLS_OUT_OF_INDEXES;
static BOOL CALLBACK init_tls(INIT_ONCE *once,void *parameter,void **context)
{
    (void)once;(void)parameter;(void)context;
    callback_tls=TlsAlloc();quit_seen_tls=TlsAlloc();quit_code_tls=TlsAlloc();
    if(callback_tls!=TLS_OUT_OF_INDEXES&&quit_seen_tls!=TLS_OUT_OF_INDEXES&&quit_code_tls!=TLS_OUT_OF_INDEXES)return TRUE;
    if(callback_tls!=TLS_OUT_OF_INDEXES)TlsFree(callback_tls);
    if(quit_seen_tls!=TLS_OUT_OF_INDEXES)TlsFree(quit_seen_tls);
    if(quit_code_tls!=TLS_OUT_OF_INDEXES)TlsFree(quit_code_tls);
    callback_tls=quit_seen_tls=quit_code_tls=TLS_OUT_OF_INDEXES;return FALSE;
}
void pw_d3d9_session_process_detach(void)
{
    if(callback_tls!=TLS_OUT_OF_INDEXES)TlsFree(callback_tls);
    if(quit_seen_tls!=TLS_OUT_OF_INDEXES)TlsFree(quit_seen_tls);
    if(quit_code_tls!=TLS_OUT_OF_INDEXES)TlsFree(quit_code_tls);
}
static unsigned callback_depth(void)
{return (unsigned)(uintptr_t)TlsGetValue(callback_tls);}
static void callback_enter(void)
{TlsSetValue(callback_tls,(void *)(uintptr_t)(callback_depth()+1));}
static void callback_leave(void)
{TlsSetValue(callback_tls,(void *)(uintptr_t)(callback_depth()-1));}
/* GetQueueStatus reads the shared queue bits without a server call when
 * nothing changed; PeekMessage would find nothing to dispatch either way.
 * It may still process driver events, so guard callbacks like the waits. */
static int client_input_pending(void)
{
    callback_enter();
    DWORD status=GetQueueStatus(QS_ALLINPUT);
    callback_leave();
    return HIWORD(status)!=0;
}
static void client_pump(void)
{
    MSG message;
    callback_enter();
    while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){
        if(message.message==WM_QUIT){TlsSetValue(quit_seen_tls,(void *)1);TlsSetValue(quit_code_tls,(void *)message.wParam);continue;}
        TranslateMessage(&message);DispatchMessageW(&message);
    }
    callback_leave();
}
static void restore_quit(void)
{if(TlsGetValue(quit_seen_tls)){int code=(int)(uintptr_t)TlsGetValue(quit_code_tls);TlsSetValue(quit_seen_tls,NULL);PostQuitMessage(code);}}
static DWORD client_wait(DWORD count,const HANDLE *handles,DWORD timeout)
{
    ULONGLONG end=GetTickCount64()+timeout;
    for(;;){
        DWORD remaining=INFINITE;
        if(timeout!=INFINITE){ULONGLONG now=GetTickCount64();if(now>=end)return WAIT_TIMEOUT;remaining=(DWORD)(end-now);}
        /* Also guard callbacks entered directly by the wait syscall, before
         * PeekMessage gets a chance to dispatch queued work. */
        callback_enter();
        DWORD result=MsgWaitForMultipleObjects(count,handles,FALSE,remaining,QS_ALLINPUT);
        callback_leave();
        if(result!=WAIT_OBJECT_0+count)return result;
        client_pump();
    }
}
#endif
static int receive_wait(struct ipc *i,struct pw_d3d9_message *m,unsigned char *scratch,size_t bytes,HANDLE peer)
{
    HANDLE waits[]={i->channel.role==PW_D3D9_CLIENT?i->reply:i->request,i->cancel,peer};
    unsigned spin=0;
    for(;;){
        int status=pw_d3d9_channel_receive(&i->channel,m,scratch,bytes);
        if(status!=PW_D3D9_EMPTY)return status;
        if(spin<PW_D3D9_SPIN_POLLS){spin++;YieldProcessor();continue;}
        /* Publish the sleeping flag, then re-check: a send published before
         * the flag became visible is received here; any later one signals. */
        pw_d3d9_channel_sleep(&i->channel,1);
        status=pw_d3d9_channel_receive(&i->channel,m,scratch,bytes);
        if(status!=PW_D3D9_EMPTY){pw_d3d9_channel_sleep(&i->channel,0);return status;}
        DWORD wait;
#if defined(_WIN64) && defined(PW_D3D9_ENABLE_DEVICE)
        wait=MsgWaitForMultipleObjects(2,waits,FALSE,INFINITE,QS_ALLINPUT);
        pw_d3d9_channel_sleep(&i->channel,0);
        if(wait==WAIT_OBJECT_0+2){MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}continue;}
#elif defined(_WIN64)
        wait=WaitForMultipleObjects(peer?3:2,waits,FALSE,peer?30000:INFINITE);
        pw_d3d9_channel_sleep(&i->channel,0);
#else
        wait=client_wait(peer?3:2,waits,peer?30000:INFINITE);
        pw_d3d9_channel_sleep(&i->channel,0);
#endif
        if(wait!=WAIT_OBJECT_0){cancel_ipc(i);return PW_D3D9_CLOSED;}
    }
}
static int send_wake(struct ipc *i,struct pw_d3d9_message *m,const void *payload)
{
    int status=pw_d3d9_channel_send(&i->channel,m,payload);
    if(status==PW_D3D9_OK && pw_d3d9_channel_peer_sleeping(&i->channel) && !SetEvent(i->channel.role==PW_D3D9_CLIENT?i->request:i->reply)){
        cancel_ipc(i);return PW_D3D9_CLOSED;
    }
    return status;
}

#ifndef _WIN64
struct bootstrap { ULONG version,size;WCHAR path[260];uint64_t result[8]; };
struct pw_d3d9_session {
    struct ipc ipc;
    HANDLE broker,serial_event;
    LONG serial_waiters;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    LONG operation_holds; /* high bit: destroy requested; low bits: active API owners */
#endif
    DWORD active_thread;
    CRITICAL_SECTION lock;
    SRWLOCK deferred_lock;
    struct pw_d3d9_deferred *deferred_first,*deferred_last;
    int draining;
    struct bootstrap bootstrap;
    LONG status;
#ifdef PW_D3D9_ENABLE_BATCH
    struct pw_d3d9_command_batch batch;
    struct pw_d3d9_object_ref batch_target;
    uint64_t batch_next,batch_started_ms;
    HRESULT batch_failure;
    int batch_exhausted,async_enabled;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    struct pw_d3d9_queue_ticket tickets[PW_D3D9_BATCH_LIMIT];
#endif
#endif
};
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
static void destroy_session_now(struct pw_d3d9_session *);
static int session_operation_hold(struct pw_d3d9_session *s)
{
    ULONG old=(ULONG)InterlockedCompareExchange(&s->operation_holds,0,0);
    for(;;){
        if((old&0x80000000u)||(old&0x7fffffffu)==0x7fffffffu)return 0;
        ULONG seen=(ULONG)InterlockedCompareExchange(&s->operation_holds,(LONG)(old+1),(LONG)old);
        if(seen==old)return 1;
        old=seen;
    }
}
struct session_operation {struct pw_d3d9_session *session;};
static void session_operation_drop(struct session_operation *guard)
{
    struct pw_d3d9_session *s=guard->session;guard->session=NULL;
    if(s&&(ULONG)InterlockedDecrement(&s->operation_holds)==0x80000000u)destroy_session_now(s);
}
#define SESSION_OPERATION(s,failure) \
    struct session_operation operation __attribute__((cleanup(session_operation_drop)))={0}; \
    if(s){if(!session_operation_hold(s))return failure;operation.session=s;}
#else
#define SESSION_OPERATION(s,failure) ((void)0)
#endif
struct batch_drops {
    size_t count;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    struct pw_d3d9_queue_ticket items[PW_D3D9_BATCH_LIMIT+1];
#endif
};
static void batch_drop(struct batch_drops *drops)
{
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    for(size_t n=0;n<drops->count;n++)pw_d3d9_queue_ticket_drop(&drops->items[n]);
#endif
    drops->count=0;
}
#ifdef PW_D3D9_ENABLE_BATCH
static void batch_detach(struct pw_d3d9_session *s,struct batch_drops *drops)
{
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    for(size_t n=0;n<s->batch.count;n++)if(s->tickets[n].drop){
        assert(drops->count<PW_D3D9_BATCH_LIMIT+1); /* one flush + one fallback/acquired ticket */
        drops->items[drops->count++]=s->tickets[n];
        s->tickets[n]=(struct pw_d3d9_queue_ticket){0};
    }
#else
    (void)s;(void)drops;
#endif
}
#endif
static LONG session_claim,session_serial;
static DWORD WINAPI broker_main(void *parameter)
{
    struct pw_d3d9_session *s=parameter;
    typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
    query_fn query=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQueryInformationProcess");
    ULONG returned=0;
    s->status=query?query(GetCurrentProcess(),0x50570001,&s->bootstrap,sizeof(s->bootstrap),&returned):(LONG)0xc0000002;
    if(!s->status && returned!=sizeof(s->bootstrap))s->status=(LONG)0xc0000004;
    return (DWORD)s->status;
}
int pw_d3d9_session_in_callback(void)
{return !!callback_depth();}
HRESULT pw_d3d9_session_defer(struct pw_d3d9_session *s,struct pw_d3d9_deferred *item)
{
    SESSION_OPERATION(s,E_FAIL);
    if(!s||!item||!item->function)return E_INVALIDARG;
    AcquireSRWLockExclusive(&s->deferred_lock);
    if(item->queued){ReleaseSRWLockExclusive(&s->deferred_lock);return E_INVALIDARG;}
    item->queued=1;item->next=NULL;
    if(s->deferred_last)s->deferred_last->next=item;else s->deferred_first=item;
    s->deferred_last=item;ReleaseSRWLockExclusive(&s->deferred_lock);return S_OK;
}
static void drain_deferred(struct pw_d3d9_session *s)
{
    AcquireSRWLockExclusive(&s->deferred_lock);
    if(s->draining){ReleaseSRWLockExclusive(&s->deferred_lock);return;}
    s->draining=1;
    for(;;){
        struct pw_d3d9_deferred *item=s->deferred_first;
        if(!item){s->draining=0;ReleaseSRWLockExclusive(&s->deferred_lock);return;}
        s->deferred_first=item->next;if(!s->deferred_first)s->deferred_last=NULL;
        item->queued=0;item->next=NULL;
        ReleaseSRWLockExclusive(&s->deferred_lock);
        item->function(item->context);
        AcquireSRWLockExclusive(&s->deferred_lock);
    }
}
static DWORD serial_enter(struct pw_d3d9_session *s)
{
    while(!TryEnterCriticalSection(&s->lock)){
        DWORD wait;
#ifdef PW_D3D9_SESSION_TEST_CALLBACK
        extern int pw_d3d9_session_test_serial_failure(void);
        if(pw_d3d9_session_test_serial_failure()){client_pump();wait=WAIT_FAILED;}
        else
#endif
        {
            /* Count before the final retry so a release that saw no waiters
             * left the lock free for this TryEnterCriticalSection. */
            InterlockedIncrement(&s->serial_waiters);
            if(TryEnterCriticalSection(&s->lock)){InterlockedDecrement(&s->serial_waiters);break;}
            wait=client_wait(1,&s->serial_event,30000);
            InterlockedDecrement(&s->serial_waiters);
        }
        if(wait!=WAIT_OBJECT_0)return wait;
    }
    return WAIT_OBJECT_0;
}
static void serial_leave(struct pw_d3d9_session *s)
{
    LeaveCriticalSection(&s->lock);
    if(InterlockedCompareExchange(&s->serial_waiters,0,0))SetEvent(s->serial_event);
}
/* Nonblocking cancellation rendezvous. The owner calls this after unlock too:
 * cancellation that loses TryEnter before that unlock cannot strand tickets.
 * Callback/inflight cancellation only marks IPC; it never drops under the gate. */
static void batch_cancel_drain(struct pw_d3d9_session *s)
{
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(callback_depth()||!s->ipc.channel.memory||!pw_d3d9_channel_error(&s->ipc.channel))return;
    if(!TryEnterCriticalSection(&s->lock))return;
    if(s->active_thread){LeaveCriticalSection(&s->lock);return;}
    struct batch_drops drops;drops.count=0;
    batch_detach(s,&drops);s->batch.count=s->batch.used=0;
    serial_leave(s);batch_drop(&drops);
#else
    (void)s;
#endif
}
#ifdef PW_D3D9_ENABLE_BATCH
/* Caller owns s->lock and active_thread; callbacks cannot enter this stream.
 * The live remote guest reference cannot retire before ordered RELEASE, which
 * passes through this same flush gate. The service pins it during replay. */
static HRESULT batch_flush_locked(struct pw_d3d9_session *s,struct batch_drops *drops)
{
    if(FAILED(s->batch_failure))return s->batch_failure;
    if(!s->batch.count)return S_OK;
    unsigned char input[PW_D3D9_BATCH_MAX],scratch[RING_BYTES];size_t bytes=0;
    struct pw_d3d9_message m={.opcode=PW_D3D9_COMMAND_BATCH_CALL,.device=1,
        .object=s->batch_target.id,.generation=s->batch_target.generation},reply={0};
    struct pw_d3d9_batch_reply decoded={0};HRESULT hr=E_FAIL;
    int prof=profile_enabled();uint64_t begin=prof?profile_now():0;
    struct pw_d3d9_transport_stats sample;struct profile_record record;
    if(prof){memset(&sample,0,sizeof(sample));sample.attempts=1;sample.opcode[PW_D3D9_COMMAND_BATCH_CALL]=1;
        uint64_t now=GetTickCount64();
        if(now<s->batch_started_ms||(now-s->batch_started_ms)>UINT64_MAX/1000u)sample.clock_invalid++;
        else sample.batch_residence_wall_us=(now-s->batch_started_ms)*1000u;}
    if(pw_d3d9_batch_encode(input,sizeof(input),&bytes,&s->batch)!=PW_D3D9_BATCH_OK)goto done;
    m.payload_bytes=(uint32_t)bytes;uint64_t sequence=s->ipc.channel.next_send;
    int sent=send_wake(&s->ipc,&m,input);
    if(prof&&s->ipc.channel.next_send!=sequence){sample.published=sample.batch_flushes=1;sample.batch_commands=s->batch.count;sample.request_bytes=64u+bytes;}
    if(sent!=PW_D3D9_OK)goto done;
    if(client_input_pending())client_pump();
    uint64_t waiting=prof?profile_now():0;
    int received=receive_wait(&s->ipc,&reply,scratch,sizeof(scratch),s->broker);
    if(prof){sample.guest_wait_wall_us=pw_d3d9_stats_elapsed(waiting,profile_now(),&sample.clock_invalid);
        if(received==PW_D3D9_OK){sample.replies=1;sample.reply_bytes=64u+reply.payload_bytes;}}
    if(received!=PW_D3D9_OK||pw_d3d9_batch_reply_decode(&decoded,scratch+64,reply.payload_bytes,
        s->batch.first_sequence,s->batch.count)!=PW_D3D9_BATCH_OK||decoded.hresult!=(uint32_t)reply.result)goto done;
    hr=(HRESULT)decoded.hresult;
 done:
    if(prof){sample.failures=FAILED(hr);sample.roundtrip_wall_us=pw_d3d9_stats_elapsed(begin,profile_now(),&sample.clock_invalid);
        profile_capture(&s->ipc,&sample,&m,0,hr,0,(int)sample.replies,0,&record);}
    if(FAILED(hr)){
        s->batch_failure=hr;cancel_ipc(&s->ipc);
        fprintf(stderr,"PW_D3D9_BATCH failed first=%llu count=%u attempted=%u failed_index=%u hr=%08lx\n",
            (unsigned long long)s->batch.first_sequence,s->batch.count,decoded.attempted,decoded.failed_index,(DWORD)hr);
    }
    batch_detach(s,drops);s->batch.count=0;s->batch.used=0;return hr;
}
static HRESULT batch_enqueue_admitted(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_command *request,
 int inherited,struct batch_drops *drops
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
 ,struct pw_d3d9_queue_ticket *ticket
#endif
 ,const struct pw_d3d9_session_observer *observer
)
{
    HRESULT hr=S_OK;int cleanup=0;
    if(!inherited){
        if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
        if(!ref.id||!ref.generation)return D3DERR_INVALIDCALL;
        if(serial_enter(s)!=WAIT_OBJECT_0){restore_quit();drain_deferred(s);return E_FAIL;}
        if(s->active_thread){LeaveCriticalSection(&s->lock);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}
        s->active_thread=GetCurrentThreadId();
    }
    cleanup=1;
    if(FAILED(s->batch_failure)){hr=s->batch_failure;goto done;}
    if(pw_d3d9_channel_state(&s->ipc.channel)!=PW_D3D9_READY){hr=E_FAIL;goto failed;}
    uint64_t now=GetTickCount64();
    if(s->batch.count&&(s->batch_target.id!=ref.id||s->batch_target.generation!=ref.generation||
        now<s->batch_started_ms||now-s->batch_started_ms>=1)){
        hr=batch_flush_locked(s,drops);if(FAILED(hr))goto done;
    }
    if(s->batch_exhausted){hr=E_FAIL;goto failed;}
 retry:
    if(!s->batch.count){
        if(pw_d3d9_batch_init(&s->batch,s->batch_next)!=PW_D3D9_BATCH_OK){hr=E_FAIL;goto failed;}
        s->batch_target=ref;s->batch_started_ms=GetTickCount64();
    }
    int appended=pw_d3d9_batch_append(&s->batch,request);
    if(appended==PW_D3D9_BATCH_FULL){hr=batch_flush_locked(s,drops);if(FAILED(hr))goto done;goto retry;}
    if(appended!=PW_D3D9_BATCH_OK){hr=E_FAIL;goto failed;}
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(ticket&&ticket->drop){s->tickets[s->batch.count-1]=*ticket;*ticket=(struct pw_d3d9_queue_ticket){0};}
#endif
    if(s->batch_next==UINT64_MAX)s->batch_exhausted=1;else s->batch_next++;
    if(observer&&observer->queued&&observer->queued(observer->context,request)!=S_OK){hr=E_FAIL;goto failed;}
    if(profile_enabled()){
        struct pw_d3d9_transport_stats sample={0};struct profile_record record;sample.async_queued=1;
        profile_capture(&s->ipc,&sample,NULL,0,S_OK,0,0,0,&record);
    }
    goto done;
 failed:
    s->batch_failure=hr;cancel_ipc(&s->ipc);
 done:
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(ticket&&ticket->drop){assert(drops->count<PW_D3D9_BATCH_LIMIT+1);drops->items[drops->count++]=*ticket;*ticket=(struct pw_d3d9_queue_ticket){0};}
#endif
    s->active_thread=0;serial_leave(s);batch_cancel_drain(s);batch_drop(drops);
    if(cleanup){restore_quit();drain_deferred(s);}return hr;
}
static HRESULT batch_enqueue(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_command *request)
{
    struct batch_drops drops;drops.count=0;
    return batch_enqueue_admitted(s,ref,request,0,&drops
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
        ,NULL
#endif
        ,NULL
    );
}
#endif
static HRESULT transact_admitted(struct pw_d3d9_session *s,struct pw_d3d9_message *m,const void *payload,
                        unsigned char *output,size_t capacity,struct pw_d3d9_message *reply,int inherited,struct batch_drops *drops,
                        const struct pw_d3d9_session_observer *observer)
{
    HRESULT result=E_FAIL;unsigned char scratch[RING_BYTES];const char *phase="send";
    int prof=profile_enabled(),present=prof?profile_present(m,payload):0,locked=0,cleanup=0,output_staged=0;
    uint64_t begin=prof?profile_now():0,serial_begin=begin,sequence=0;
    struct pw_d3d9_transport_stats sample;struct profile_record record;
    if(prof){memset(&sample,0,sizeof(sample));sample.attempts=1;if(m->opcode<PW_D3D9_STATS_OPS)sample.opcode[m->opcode]=1;}
    memset(reply,0,sizeof(*reply));
    if(!inherited){
        if(callback_depth()){result=RPC_E_CANTCALLOUT_ININPUTSYNCCALL;goto metric_done;}
        if(serial_enter(s)!=WAIT_OBJECT_0){cleanup=1;if(prof)sample.serial_wait_wall_us=pw_d3d9_stats_elapsed(serial_begin,profile_now(),&sample.clock_invalid);goto metric_done;}
        if(prof)sample.serial_wait_wall_us=pw_d3d9_stats_elapsed(serial_begin,profile_now(),&sample.clock_invalid);
        if(s->active_thread){LeaveCriticalSection(&s->lock);result=RPC_E_CANTCALLOUT_ININPUTSYNCCALL;goto metric_done;}
        s->active_thread=GetCurrentThreadId();
    }
    locked=cleanup=1;
#ifdef PW_D3D9_ENABLE_BATCH
    int timed_flush=prof&&s->batch.count;
    if(timed_flush)sample.roundtrip_wall_us=pw_d3d9_stats_elapsed(begin,profile_now(),&sample.clock_invalid);
    result=batch_flush_locked(s,drops);
    if(timed_flush)begin=profile_now(); /* batch sample owns the excluded flush interval */
    if(FAILED(result))goto done;
    if(m->opcode==PW_D3D9_STOP&&pw_d3d9_channel_stop(&s->ipc.channel)!=PW_D3D9_OK){result=E_FAIL;goto done;}
    result=E_FAIL;
#endif
    sequence=s->ipc.channel.next_send;
    int sent=send_wake(&s->ipc,m,payload);
    if(prof&&s->ipc.channel.next_send!=sequence){sample.published=1;sample.request_bytes=64u+m->payload_bytes;}
    if(sent!=PW_D3D9_OK)goto done;
#ifdef PW_D3D9_SESSION_TEST_CALLBACK
    extern void pw_d3d9_session_test_callback(void);
    pw_d3d9_session_test_callback();
#endif
    if(client_input_pending())client_pump();
    phase="receive";
    uint64_t wait_begin=prof?profile_now():0;
    int received=receive_wait(&s->ipc,reply,scratch,sizeof(scratch),s->broker);
    if(prof){sample.guest_wait_wall_us=pw_d3d9_stats_elapsed(wait_begin,profile_now(),&sample.clock_invalid);
        if(received==PW_D3D9_OK){sample.replies=1;sample.reply_bytes=64u+reply->payload_bytes;}}
    if(received!=PW_D3D9_OK)goto done;
    phase="reply_capacity";
    if(reply->payload_bytes>capacity){cancel_ipc(&s->ipc);goto done;}
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(m->opcode==PW_D3D9_COMMAND_CALL&&!(FAILED((HRESULT)reply->result)&&!reply->payload_bytes)){
        uint32_t method,hresult;
        if(m->payload_bytes<8||!payload||pw_d3d9_command_reply_decode(&method,&hresult,scratch+64,reply->payload_bytes)!=PW_D3D9_COMMAND_OK||
           method!=get32((const unsigned char *)payload+4)||hresult!=(uint32_t)reply->result){
            cancel_ipc(&s->ipc);reply->payload_bytes=0;goto done;
        }
    }
#endif
    if(observer&&observer->completed&&observer->completed(observer->context,m->opcode,payload,m->payload_bytes,
        scratch+64,reply->payload_bytes,(HRESULT)reply->result)!=S_OK){cancel_ipc(&s->ipc);goto done;}
    if(reply->payload_bytes)memcpy(output,scratch+64,reply->payload_bytes);
    output_staged=1;
    phase="backend_result";result=(HRESULT)reply->result;
 done:
    if(!output_staged)reply->payload_bytes=0;
    if(FAILED(result))fprintf(stderr,"PW_D3D9 transaction opcode=%u phase=%s hr=%08lx reply_bytes=%u\n",m->opcode,phase,(DWORD)result,reply->payload_bytes);
 metric_done:
    if(prof){sample.rejected_present=present&&!sample.published;sample.failures=FAILED(result);sample.roundtrip_wall_us=pw_d3d9_stats_sum(sample.roundtrip_wall_us,pw_d3d9_stats_elapsed(begin,profile_now(),&sample.clock_invalid),&sample.saturated);}
    if(prof)profile_capture(&s->ipc,&sample,m,sample.published?sequence:0,result,present&&sample.published,(int)sample.replies,0,&record);
    if(locked){
        s->active_thread=0;serial_leave(s);
    }
    if(prof)profile_emit(&record);
#ifdef PW_D3D9_ENABLE_API_OBSERVE
    if(prof&&present&&sample.published)
        pw_d3d9_api_profile_present(s->ipc.channel.epoch,sequence,m->object,m->generation,result);
#endif
    if(locked){batch_cancel_drain(s);batch_drop(drops);}
    if(cleanup){restore_quit();drain_deferred(s);}return result;
}
static HRESULT transact(struct pw_d3d9_session *s,struct pw_d3d9_message *m,const void *payload,
 unsigned char *output,size_t capacity,struct pw_d3d9_message *reply)
{
    struct batch_drops drops;drops.count=0;
    return transact_admitted(s,m,payload,output,capacity,reply,0,&drops,NULL);
}
static void destroy_session_now(struct pw_d3d9_session *s)
{
    if(s->broker){client_wait(1,&s->broker,INFINITE);CloseHandle(s->broker);}
    if(s->serial_event)CloseHandle(s->serial_event);
#ifdef PW_D3D9_ENABLE_API_OBSERVE
    pw_d3d9_api_profile_flush(s->ipc.channel.epoch);
#endif
    restore_quit();close_ipc(&s->ipc);DeleteCriticalSection(&s->lock);HeapFree(GetProcessHeap(),0,s);
    InterlockedExchange(&session_claim,0);
}
static void destroy_session(struct pw_d3d9_session *s)
{
    batch_cancel_drain(s);
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(!(ULONG)InterlockedOr(&s->operation_holds,(LONG)0x80000000u))destroy_session_now(s);
#else
    destroy_session_now(s);
#endif
}
HRESULT pw_d3d9_session_open(const WCHAR *service,const WCHAR *backend,struct pw_d3d9_session **out)
{
    WCHAR object_name[96];struct pw_d3d9_session *s;DWORD pid=GetCurrentProcessId(),epoch;
    unsigned char payload[32],reply_bytes[32];struct pw_d3d9_message m={.opcode=PW_D3D9_HELLO,.payload_bytes=32},r;
    if(!out)return E_POINTER;
    *out=NULL;
    if(!InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL))return E_OUTOFMEMORY;
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    if(!absolute_path(service)||!absolute_path(backend))return E_INVALIDARG;
    if(InterlockedCompareExchange(&session_claim,1,0))return HRESULT_FROM_WIN32(ERROR_BUSY);
    if(session_serial==0x7fffffff){InterlockedExchange(&session_claim,0);return E_OUTOFMEMORY;}
    epoch=(DWORD)++session_serial;
    s=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*s));
    if(!s){InterlockedExchange(&session_claim,0);return E_OUTOFMEMORY;}
#ifdef PW_D3D9_ENABLE_BATCH
    s->batch_next=1;
    {char value[2];s->async_enabled=GetEnvironmentVariableA("PW_D3D9_ASYNC",value,sizeof(value))==1&&value[0]=='1';}
#endif
    InitializeCriticalSection(&s->lock);
    InitializeSRWLock(&s->deferred_lock);
    s->serial_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!s->serial_event)goto fail;
    name(object_name,L"BOOT",pid,0);
    s->ipc.descriptor_mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(struct descriptor),object_name);
    if(!s->ipc.descriptor_mapping || GetLastError()==ERROR_ALREADY_EXISTS)goto fail;
    s->ipc.descriptor=MapViewOfFile(s->ipc.descriptor_mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(struct descriptor));
    if(!s->ipc.descriptor)goto fail;
    name(object_name,L"WIRE",pid,epoch);
    s->ipc.wire_mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES),object_name);
    if(!s->ipc.wire_mapping || GetLastError()==ERROR_ALREADY_EXISTS)goto fail;
    s->ipc.memory=MapViewOfFile(s->ipc.wire_mapping,FILE_MAP_ALL_ACCESS,0,0,pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES));
    if(!s->ipc.memory || pw_d3d9_channel_init(s->ipc.memory,pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES),epoch,RING_BYTES,RING_BYTES)!=PW_D3D9_OK ||
       pw_d3d9_channel_open(&s->ipc.channel,s->ipc.memory,pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES),epoch,PW_D3D9_CLIENT)!=PW_D3D9_OK)goto fail;
    const WCHAR *kinds[]={L"REQUEST",L"REPLY",L"OPEN",L"CANCEL"};
    HANDLE *events[]={&s->ipc.request,&s->ipc.reply,&s->ipc.opened,&s->ipc.cancel};
    for(unsigned n=0;n<4;n++){
        name(object_name,kinds[n],pid,epoch);*events[n]=CreateEventW(NULL,n>=2,FALSE,object_name);
        if(!*events[n] || GetLastError()==ERROR_ALREADY_EXISTS)goto fail;
    }
    *s->ipc.descriptor=(struct descriptor){SESSION_MAGIC,1,sizeof(struct descriptor),epoch,pid,RING_BYTES,BACKEND_CRC,BACKEND_BYTES,{0}};
    wcscpy(s->ipc.descriptor->backend,backend);
    s->bootstrap.version=1;s->bootstrap.size=sizeof(s->bootstrap);wcscpy(s->bootstrap.path,service);
    MemoryBarrier();s->broker=CreateThread(NULL,0,broker_main,s,0,NULL);
    if(!s->broker)goto fail;
    HANDLE waits[]={s->ipc.opened,s->broker};
    if(client_wait(2,waits,30000)!=WAIT_OBJECT_0)goto fail;
    hello_payload(payload,epoch);
    if(FAILED(transact(s,&m,payload,reply_bytes,sizeof(reply_bytes),&r)) || r.payload_bytes!=32 || memcmp(payload,reply_bytes,32))goto fail;
    *out=s;restore_quit();return S_OK;
 fail:
    cancel_ipc(&s->ipc);destroy_session(s);return E_FAIL;
}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
static HRESULT transact_observed(struct pw_d3d9_session *s,struct pw_d3d9_message *m,const void *payload,
 unsigned char *output,size_t capacity,struct pw_d3d9_message *reply,const struct pw_d3d9_session_observer *observer)
{
    struct batch_drops drops;drops.count=0;
    if(!observer||!observer->completed){memset(reply,0,sizeof(*reply));pw_d3d9_session_cancel(s);return E_FAIL;}
    return transact_admitted(s,m,payload,output,capacity,reply,0,&drops,observer);
}
#endif
HRESULT pw_d3d9_session_create(struct pw_d3d9_session *s,UINT sdk,struct pw_d3d9_object_ref *ref)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[4],out[8];struct pw_d3d9_message m={.opcode=PW_D3D9_CREATE9,.device=1,.payload_bytes=4},r;
    if(!s||!ref)return E_POINTER;
    *ref=(struct pw_d3d9_object_ref){0};put32(in,sdk);
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);if(FAILED(hr))return hr;
    if(r.payload_bytes!=8 || !get32(out) || !get32(out+4)){cancel_ipc(&s->ipc);return E_FAIL;}
    *ref=(struct pw_d3d9_object_ref){get32(out),get32(out+4)};return hr;
}
HRESULT pw_d3d9_session_factory(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
                               const struct pw_d3d9_factory_request *request,struct pw_d3d9_factory_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[128],out[PW_D3D9_FACTORY_MAX_REPLY];size_t bytes;
    struct pw_d3d9_message m={.opcode=PW_D3D9_FACTORY_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    int encoded=pw_d3d9_factory_request_encode(in,sizeof(in),&bytes,request);
    if(encoded==PW_D3D9_FACTORY_UNSUPPORTED)return E_NOTIMPL;
    if(encoded!=PW_D3D9_FACTORY_OK)return E_INVALIDARG;
    m.payload_bytes=(uint32_t)bytes;
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    /* A target failure has no typed result (especially no manufactured count). */
    if(!r.payload_bytes && FAILED(hr))return hr;
    struct pw_d3d9_factory_reply decoded;
    if(pw_d3d9_factory_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_FACTORY_OK || decoded.method!=request->method || decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;
    return hr;
}
#ifdef PW_D3D9_ENABLE_DEVICE
HRESULT pw_d3d9_session_device(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
                              const struct pw_d3d9_device_request *request,struct pw_d3d9_device_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_DEVICE_MAX_REQUEST],out[PW_D3D9_DEVICE_MAX_REPLY];size_t bytes;
    struct pw_d3d9_message m={.opcode=PW_D3D9_DEVICE_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    int encoded=pw_d3d9_device_request_encode(in,sizeof(in),&bytes,request);
    if(encoded==PW_D3D9_DEVICE_UNSUPPORTED)return E_NOTIMPL;
    if(encoded!=PW_D3D9_DEVICE_OK)return E_INVALIDARG;
    m.payload_bytes=(uint32_t)bytes;
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(!r.payload_bytes&&FAILED(hr))return hr;
    struct pw_d3d9_device_reply decoded;
    if(pw_d3d9_device_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_DEVICE_OK || decoded.operation!=request->operation || decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
HRESULT pw_d3d9_session_device_observed(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
                              const struct pw_d3d9_device_request *request,struct pw_d3d9_device_reply *reply,const struct pw_d3d9_session_observer *observer)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_DEVICE_MAX_REQUEST],out[PW_D3D9_DEVICE_MAX_REPLY];size_t bytes;
    struct pw_d3d9_message m={.opcode=PW_D3D9_DEVICE_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    int encoded=pw_d3d9_device_request_encode(in,sizeof(in),&bytes,request);
    if(encoded==PW_D3D9_DEVICE_UNSUPPORTED)return E_NOTIMPL;
    if(encoded!=PW_D3D9_DEVICE_OK)return E_INVALIDARG;
    m.payload_bytes=(uint32_t)bytes;
    HRESULT hr=transact_observed(s,&m,in,out,sizeof(out),&r,observer);
    if(!r.payload_bytes&&FAILED(hr))return hr;
    struct pw_d3d9_device_reply decoded;
    if(pw_d3d9_device_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_DEVICE_OK || decoded.operation!=request->operation || decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif

#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
HRESULT pw_d3d9_session_resource(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_resource_request *request,struct pw_d3d9_resource_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_RESOURCE_MAX_WIRE],out[PW_D3D9_RESOURCE_MAX_WIRE];size_t bytes;
    struct pw_d3d9_resource_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_RESOURCE_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_resource_request_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_RESOURCE_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_resource_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_RESOURCE_OK||decoded.operation!=request->operation||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
HRESULT pw_d3d9_session_texture(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_texture_request *request,struct pw_d3d9_texture_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_TEXTURE_MAX_WIRE],out[PW_D3D9_TEXTURE_MAX_WIRE];size_t bytes;
    struct pw_d3d9_texture_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_TEXTURE_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_texture_request_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_RESOURCE_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_texture_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_RESOURCE_OK||decoded.operation!=request->operation||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_METHODS
HRESULT pw_d3d9_session_command(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_command *request)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[RING_BYTES],out[16];size_t bytes;uint32_t method,hresult;
    struct pw_d3d9_message m={.opcode=PW_D3D9_COMMAND_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request)return E_POINTER;
#ifdef PW_D3D9_ENABLE_BATCH
    if(s->async_enabled&&pw_d3d9_command_can_queue(request))return batch_enqueue(s,ref,request);
#endif
    if(pw_d3d9_command_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_COMMAND_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_command_reply_decode(&method,&hresult,out,r.payload_bytes)!=PW_D3D9_COMMAND_OK||method!=request->method||hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    return hr;
}
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
HRESULT pw_d3d9_session_binding(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 struct pw_d3d9_command *request,pw_d3d9_binding_acquire_fn acquire,void *context)
{
    SESSION_OPERATION(s,E_FAIL);
    if(!s||!request||!acquire)return E_POINTER;
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    if(!ref.id||!ref.generation)return D3DERR_INVALIDCALL;
    if(serial_enter(s)!=WAIT_OBJECT_0){restore_quit();drain_deferred(s);return E_FAIL;}
    if(s->active_thread){LeaveCriticalSection(&s->lock);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}
    s->active_thread=GetCurrentThreadId();
    struct batch_drops drops;drops.count=0;
    struct pw_d3d9_queue_ticket ticket={0};HRESULT hr=E_FAIL;
    if(pw_d3d9_channel_state(&s->ipc.channel)!=PW_D3D9_READY)goto failed;
    hr=acquire(context,request,&ticket);
    if(hr!=S_OK){if(SUCCEEDED(hr))hr=E_FAIL;goto failed;}
    struct pw_d3d9_binding plan;
    if(s->async_enabled&&pw_d3d9_binding_plan(request,&plan)==PW_D3D9_BINDING_READY)
        return batch_enqueue_admitted(s,ref,request,1,&drops,&ticket,NULL);
    /* Synchronous fallback keeps the same gate and ticket through the reply. */
    unsigned char input[RING_BYTES],output[16];size_t bytes;uint32_t method,result;
    if(pw_d3d9_command_encode(input,sizeof(input),&bytes,request)!=PW_D3D9_COMMAND_OK){hr=D3DERR_INVALIDCALL;goto failed;}
    if(ticket.drop){drops.items[drops.count++]=ticket;ticket=(struct pw_d3d9_queue_ticket){0};}
    struct pw_d3d9_message m={.opcode=PW_D3D9_COMMAND_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=(uint32_t)bytes},r;
    hr=transact_admitted(s,&m,input,output,sizeof(output),&r,1,&drops,NULL);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_command_reply_decode(&method,&result,output,r.payload_bytes)!=PW_D3D9_COMMAND_OK||method!=request->method||result!=(uint32_t)hr){pw_d3d9_session_cancel(s);return E_FAIL;}
    return hr;
 failed:
    if(ticket.drop)drops.items[drops.count++]=ticket;
    s->active_thread=0;serial_leave(s);batch_cancel_drain(s);batch_drop(&drops);
    restore_quit();drain_deferred(s);return hr;
}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
HRESULT pw_d3d9_session_binding_observed(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 struct pw_d3d9_command *request,pw_d3d9_binding_acquire_fn acquire,void *context,const struct pw_d3d9_session_observer *observer)
{
    SESSION_OPERATION(s,E_FAIL);
    if(!s||!request||!acquire)return E_POINTER;
    if(!observer||!observer->completed||!observer->queued){pw_d3d9_session_cancel(s);return E_FAIL;}
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    if(!ref.id||!ref.generation)return D3DERR_INVALIDCALL;
    if(serial_enter(s)!=WAIT_OBJECT_0){restore_quit();drain_deferred(s);return E_FAIL;}
    if(s->active_thread){LeaveCriticalSection(&s->lock);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}
    s->active_thread=GetCurrentThreadId();
    struct batch_drops drops;drops.count=0;
    struct pw_d3d9_queue_ticket ticket={0};HRESULT hr=E_FAIL;
    if(pw_d3d9_channel_state(&s->ipc.channel)!=PW_D3D9_READY)goto failed;
    hr=acquire(context,request,&ticket);
    if(hr!=S_OK){if(SUCCEEDED(hr))hr=E_FAIL;goto failed;}
    struct pw_d3d9_binding plan;
    if(s->async_enabled&&pw_d3d9_binding_plan(request,&plan)==PW_D3D9_BINDING_READY)
        return batch_enqueue_admitted(s,ref,request,1,&drops,&ticket,observer);
    /* Synchronous fallback keeps the same gate and ticket through the reply. */
    unsigned char input[RING_BYTES],output[16];size_t bytes;uint32_t method,result;
    if(pw_d3d9_command_encode(input,sizeof(input),&bytes,request)!=PW_D3D9_COMMAND_OK){hr=D3DERR_INVALIDCALL;goto failed;}
    if(ticket.drop){drops.items[drops.count++]=ticket;ticket=(struct pw_d3d9_queue_ticket){0};}
    struct pw_d3d9_message m={.opcode=PW_D3D9_COMMAND_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=(uint32_t)bytes},r;
    hr=transact_admitted(s,&m,input,output,sizeof(output),&r,1,&drops,observer);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_command_reply_decode(&method,&result,output,r.payload_bytes)!=PW_D3D9_COMMAND_OK||method!=request->method||result!=(uint32_t)hr){pw_d3d9_session_cancel(s);return E_FAIL;}
    return hr;
 failed:
    if(ticket.drop)drops.items[drops.count++]=ticket;
    s->active_thread=0;serial_leave(s);batch_cancel_drain(s);batch_drop(&drops);
    restore_quit();drain_deferred(s);return hr;
}
#endif

#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
HRESULT pw_d3d9_session_command_observed(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_command *request,const struct pw_d3d9_session_observer *observer)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char input[RING_BYTES],output[16];size_t bytes;uint32_t method,result;
    if(!s||!request)return E_POINTER;
    if(!observer||!observer->completed||!observer->queued||!observer->eligible){pw_d3d9_session_cancel(s);return E_FAIL;}
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    if(!ref.id||!ref.generation)return D3DERR_INVALIDCALL;
    if(pw_d3d9_command_encode(input,sizeof(input),&bytes,request)!=PW_D3D9_COMMAND_OK)return D3DERR_INVALIDCALL;
    if(serial_enter(s)!=WAIT_OBJECT_0){restore_quit();drain_deferred(s);return E_FAIL;}
    if(s->active_thread){LeaveCriticalSection(&s->lock);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}
    s->active_thread=GetCurrentThreadId();
    struct batch_drops drops;drops.count=0;
    /* Read owner evidence only after admission. No local cache or successful
     * acceptance may bypass cancellation, sticky failure or callback guards. */
    if(pw_d3d9_channel_state(&s->ipc.channel)==PW_D3D9_READY&&SUCCEEDED(s->batch_failure)&&s->async_enabled&&
       (pw_d3d9_command_can_queue(request)||observer->eligible(observer->context,request)))
        return batch_enqueue_admitted(s,ref,request,1,&drops,NULL,observer);
    struct pw_d3d9_message m={.opcode=PW_D3D9_COMMAND_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=(uint32_t)bytes},r;
    HRESULT hr=transact_admitted(s,&m,input,output,sizeof(output),&r,1,&drops,observer);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_command_reply_decode(&method,&result,output,r.payload_bytes)!=PW_D3D9_COMMAND_OK||method!=request->method||result!=(uint32_t)hr){pw_d3d9_session_cancel(s);return E_FAIL;}
    return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
HRESULT pw_d3d9_session_getter_observed(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_getter_request *request,struct pw_d3d9_getter_reply *reply,const struct pw_d3d9_session_observer *observer)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char input[24],output[PW_D3D9_GETTER_MAX];size_t bytes,written;struct pw_d3d9_getter_reply decoded={0};
    if(!s||!request||!reply)return E_POINTER;
    if(!observer||!observer->completed){pw_d3d9_session_cancel(s);return E_FAIL;}
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    if(!ref.id||!ref.generation)return D3DERR_INVALIDCALL;
    if(pw_d3d9_getter_encode(input,sizeof(input),&bytes,request)!=PW_D3D9_GETTER_OK)return D3DERR_INVALIDCALL;
    if(serial_enter(s)!=WAIT_OBJECT_0){restore_quit();drain_deferred(s);return E_FAIL;}
    if(s->active_thread){LeaveCriticalSection(&s->lock);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}
    s->active_thread=GetCurrentThreadId();struct batch_drops drops;drops.count=0;HRESULT hr=E_FAIL;
    if(FAILED(s->batch_failure)){hr=s->batch_failure;goto local_done;}
    if(pw_d3d9_channel_state(&s->ipc.channel)!=PW_D3D9_READY)goto local_done;
    /* A known answer observes accepted ordered setters. It may avoid flushing
     * a pending proven batch, but can never bypass an already known failure.
     * Async-off remains the original synchronous baseline. */
    if(s->async_enabled&&request->method==45&&observer->answer){
        int hit=observer->answer(observer->context,request,&decoded);
        if(hit<0){cancel_ipc(&s->ipc);goto local_done;}
        if(hit){
            if(hit!=1||decoded.hresult!=S_OK||pw_d3d9_getter_reply_encode(output,sizeof(output),&written,request,&decoded)!=PW_D3D9_GETTER_OK){cancel_ipc(&s->ipc);goto local_done;}
            hr=S_OK;goto local_done;
        }
    }
    struct pw_d3d9_message m={.opcode=PW_D3D9_GETTER_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=(uint32_t)bytes},r;
    hr=transact_admitted(s,&m,input,output,sizeof(output),&r,1,&drops,observer);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_getter_reply_decode(&decoded,request,output,r.payload_bytes)!=PW_D3D9_GETTER_OK||decoded.hresult!=(uint32_t)hr){pw_d3d9_session_cancel(s);return E_FAIL;}
    *reply=decoded;return hr;
 local_done:
    s->active_thread=0;serial_leave(s);batch_cancel_drain(s);batch_drop(&drops);
    restore_quit();drain_deferred(s);
    if(hr==S_OK)*reply=decoded;
    return hr;
}
#endif
HRESULT pw_d3d9_session_getter(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_getter_request *request,struct pw_d3d9_getter_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[24],out[PW_D3D9_GETTER_MAX];size_t bytes;struct pw_d3d9_getter_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_GETTER_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_getter_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_GETTER_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_getter_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_GETTER_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
HRESULT pw_d3d9_session_program_query(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_program_query_request *request,struct pw_d3d9_program_query_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[24],out[PW_D3D9_PROGRAM_CHUNK+32];size_t bytes;struct pw_d3d9_program_query_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_PROGRAM_QUERY_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_program_query_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_PROGRAM_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_program_query_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_PROGRAM_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
HRESULT pw_d3d9_session_program(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_program_request *request,struct pw_d3d9_program_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_PROGRAM_WIRE_MAX],out[32];size_t bytes;struct pw_d3d9_program_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_PROGRAM_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_program_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_PROGRAM_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_program_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_PROGRAM_OK||decoded.operation!=request->operation||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
HRESULT pw_d3d9_session_gamma(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_gamma_request *request,struct pw_d3d9_gamma_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_GAMMA_REQUEST_BYTES],out[PW_D3D9_GAMMA_REPLY_BYTES];struct pw_d3d9_gamma_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_GAMMA_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=sizeof(in)},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_gamma_request_encode(in,sizeof(in),request))return D3DERR_INVALIDCALL;
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_gamma_reply_decode(&decoded,request,out,r.payload_bytes)||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_CURSOR
HRESULT pw_d3d9_session_cursor(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_cursor_request *request,struct pw_d3d9_cursor_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[32],out[16];size_t bytes;struct pw_d3d9_cursor_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_CURSOR_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_cursor_encode(in,sizeof(in),&bytes,request))return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_cursor_reply_decode(&decoded,out,r.payload_bytes)||decoded.method!=request->method||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_QUERY
HRESULT pw_d3d9_session_query(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_query_request *request,struct pw_d3d9_query_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_QUERY_WIRE_MAX],out[PW_D3D9_QUERY_WIRE_MAX];size_t bytes;struct pw_d3d9_query_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_QUERY_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_query_request_encode(in,sizeof(in),&bytes,request))return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_query_reply_decode(&decoded,request,out,r.payload_bytes)||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    /* Failed backend polls may still modify output bytes; publish the validated
     * reply for every backend HRESULT, including S_FALSE and failures. */
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
HRESULT pw_d3d9_session_stateblock(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *request,struct pw_d3d9_stateblock_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[16],out[16];struct pw_d3d9_stateblock_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_STATEBLOCK_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=16},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_stateblock_encode(in,sizeof(in),request)!=PW_D3D9_SB_OK)return D3DERR_INVALIDCALL;
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_stateblock_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_SB_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
HRESULT pw_d3d9_session_stateblock_observed(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *request,struct pw_d3d9_stateblock_reply *reply,const struct pw_d3d9_session_observer *observer)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[16],out[16];struct pw_d3d9_stateblock_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_STATEBLOCK_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=16},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_stateblock_encode(in,sizeof(in),request)!=PW_D3D9_SB_OK)return D3DERR_INVALIDCALL;
    HRESULT hr=transact_observed(s,&m,in,out,sizeof(out),&r,observer);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_stateblock_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_SB_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif

#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
HRESULT pw_d3d9_session_object_getter(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_object_getter_request *request,struct pw_d3d9_object_getter_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[24],out[32];size_t bytes;struct pw_d3d9_object_getter_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_OBJECT_GETTER_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_object_getter_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_OBJECT_GETTER_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_object_getter_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_OBJECT_GETTER_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
HRESULT pw_d3d9_session_object_getter_observed(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_object_getter_request *request,struct pw_d3d9_object_getter_reply *reply,const struct pw_d3d9_session_observer *observer)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[24],out[32];size_t bytes;struct pw_d3d9_object_getter_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_OBJECT_GETTER_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_object_getter_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_OBJECT_GETTER_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact_observed(s,&m,in,out,sizeof(out),&r,observer);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_object_getter_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_OBJECT_GETTER_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif

#endif
#ifdef PW_D3D9_ENABLE_UP
HRESULT pw_d3d9_session_up(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_up_request *request,struct pw_d3d9_up_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_UP_WIRE_MAX],out[24];size_t bytes;struct pw_d3d9_up_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_UP_DRAW_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_up_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_UP_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_up_reply_decode(&decoded,out,r.payload_bytes)!=PW_D3D9_UP_OK||decoded.operation!=request->operation||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
#ifdef PW_D3D9_ENABLE_IMPLICIT
HRESULT pw_d3d9_session_implicit(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_implicit_request *request,struct pw_d3d9_implicit_reply *reply)
{
    SESSION_OPERATION(s,E_FAIL);
    unsigned char in[PW_D3D9_IMPLICIT_REQUEST_BYTES],out[PW_D3D9_IMPLICIT_REPLY_BYTES];
    struct pw_d3d9_implicit_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_IMPLICIT_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=sizeof(in)},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_implicit_request_encode(in,sizeof(in),request))return D3DERR_INVALIDCALL;
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_implicit_reply_decode(&decoded,request,out,r.payload_bytes)||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref)
{
    SESSION_OPERATION(s,E_FAIL);
    struct pw_d3d9_message m={.opcode=PW_D3D9_RELEASE,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s)return E_POINTER;
    return transact(s,&m,NULL,NULL,0,&r);
}
void pw_d3d9_session_cancel(struct pw_d3d9_session *s)
{
    SESSION_OPERATION(s,); if(s){cancel_ipc(&s->ipc);batch_cancel_drain(s);} }
HRESULT pw_d3d9_session_join(struct pw_d3d9_session *s)
{
    SESSION_OPERATION(s,E_FAIL);
    if(!s)return E_POINTER;
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(s->broker&&GetThreadId(s->broker)==GetCurrentThreadId())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
#endif
    DWORD wait=client_wait(1,&s->broker,INFINITE);restore_quit();
    return wait==WAIT_OBJECT_0&&!s->status?S_OK:E_FAIL;
}
HRESULT pw_d3d9_session_close(struct pw_d3d9_session *s)
{
    SESSION_OPERATION(s,E_FAIL);
    struct pw_d3d9_message m={.opcode=PW_D3D9_STOP},r;HRESULT hr=E_FAIL;
    if(!s)return E_POINTER;
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
    if(s->broker&&GetThreadId(s->broker)==GetCurrentThreadId())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
#endif
#ifdef PW_D3D9_ENABLE_BATCH
    hr=transact(s,&m,NULL,NULL,0,&r); /* Flush while READY, then STOP under the same lock. */
#else
    if(pw_d3d9_channel_stop(&s->ipc.channel)==PW_D3D9_OK)hr=transact(s,&m,NULL,NULL,0,&r);
#endif
    if(FAILED(hr))cancel_ipc(&s->ipc);
    client_wait(1,&s->broker,INFINITE);
    if(s->status || pw_d3d9_channel_state(&s->ipc.channel)!=PW_D3D9_STOPPED)hr=E_FAIL;
    destroy_session(s);return hr;
}
#else
/* Native service implementation follows the same named-object contract. */
static HMODULE load_backend(const WCHAR *path)
{
    typedef ULONG (WINAPI *crc_fn)(ULONG,const void *,INT);
    crc_fn crc=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlComputeCrc32");
    unsigned char bytes[16384];DWORD read,total=0;ULONG value=0;HMODULE module=NULL;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE || !crc){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return NULL;}
    for(;;){
        if(!ReadFile(file,bytes,sizeof(bytes),&read,NULL))goto done;
        if(!read)break;
        if(read>BACKEND_BYTES-total)goto done;
        total+=read;value=crc(value,bytes,(INT)read);
    }
    if(total==BACKEND_BYTES && value==BACKEND_CRC)module=LoadLibraryW(path);
 done:
    CloseHandle(file);return module;
}
static int destroy_objects(struct pw_d3d9_objects *objects)
{
    int okay=1,progress;
    do { progress=0;
    for(uint32_t n=0;n<objects->capacity;n++){
        struct pw_d3d9_object_ref ref={n+1,objects->slots[n].generation};uintptr_t context;
        if(pw_d3d9_object_take_destroy(objects,ref,&context)){
            progress=1;
            if(objects->slots[n].kind==1)IDirect3D9_Release((IDirect3D9 *)context);
#ifdef PW_D3D9_ENABLE_RESOURCE
            else if(objects->slots[n].kind==PW_D3D9_KIND_VERTEX_BUFFER||objects->slots[n].kind==PW_D3D9_KIND_INDEX_BUFFER){if(FAILED(pw_d3d9_service_resource_destroy(objects,context)))okay=0;}
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
            else if(objects->slots[n].kind==5||objects->slots[n].kind==6){if(FAILED(pw_d3d9_service_texture_destroy(objects,context)))okay=0;}
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
            else if(objects->slots[n].kind>=7&&objects->slots[n].kind<=9){if(FAILED(pw_d3d9_service_program_destroy(objects,context)))okay=0;}
#endif
#ifdef PW_D3D9_ENABLE_QUERY
            else if(objects->slots[n].kind==PW_D3D9_KIND_QUERY){if(FAILED(pw_d3d9_service_query_destroy(objects,context)))okay=0;}
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
            else if(objects->slots[n].kind==10){if(FAILED(pw_d3d9_service_stateblock_destroy(objects,context)))okay=0;}
#endif
#ifdef PW_D3D9_ENABLE_DEVICE
            else if(objects->slots[n].kind==2){
#ifdef PW_D3D9_ENABLE_PROGRAM
                pw_d3d9_service_program_retire(objects,ref);
#endif
#ifdef PW_D3D9_ENABLE_UP
                pw_d3d9_service_up_retire(objects,ref);
#endif
                if(!pw_d3d9_native_device_destroy((void *)context))okay=0;}
            else okay=0;
#endif
            pw_d3d9_object_finish_destroy(objects,ref);
        }
    }
    } while(progress);
    return okay;
}
#ifdef PW_D3D9_ENABLE_IMPLICIT
static HRESULT implicit_call(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_implicit_request *q,struct pw_d3d9_implicit_reply *r)
{
    memset(r,0,sizeof(*r));r->operation=q->operation;r->hresult=D3DERR_INVALIDCALL;
    const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(objects,objects->device,objects->epoch,ref);
    if(!slot||slot->kind!=PW_D3D9_KIND_DEVICE||!pw_d3d9_object_queue(objects,ref))return r->hresult;
    struct pw_d3d9_native_device *d=(void *)slot->context;
    unsigned phase=pw_d3d9_native_device_implicit_phase(d);HRESULT hr=D3DERR_INVALIDCALL;UINT count=0;
    if(q->operation==PW_D3D9_IMPLICIT_LIST&&phase==PW_D3D9_IMPLICIT_IDLE){
        hr=pw_d3d9_service_texture_owners_list(objects,ref,r->objects,PW_D3D9_IMPLICIT_MAX,&count);
    }else if(q->operation==PW_D3D9_IMPLICIT_PREPARE&&phase==PW_D3D9_IMPLICIT_IDLE){
        hr=pw_d3d9_service_texture_owners_prepare(objects,ref,q->objects,q->count);
        if(SUCCEEDED(hr))pw_d3d9_native_device_implicit_set_phase(d,PW_D3D9_IMPLICIT_PREPARED);
    }else if(q->operation==PW_D3D9_IMPLICIT_FINISH&&
             (phase==PW_D3D9_IMPLICIT_DONE_RESTORED||phase==PW_D3D9_IMPLICIT_DONE_RETIRED)){
        hr=pw_d3d9_service_texture_owners_list(objects,ref,r->objects,PW_D3D9_IMPLICIT_MAX,&count);
        if(SUCCEEDED(hr)){
            r->disposition=phase==PW_D3D9_IMPLICIT_DONE_RESTORED?PW_D3D9_IMPLICIT_RESTORED:PW_D3D9_IMPLICIT_RETIRED;
            pw_d3d9_native_device_implicit_set_phase(d,PW_D3D9_IMPLICIT_IDLE);
        }
    }else if(q->operation==PW_D3D9_IMPLICIT_DRAIN&&phase==PW_D3D9_IMPLICIT_IDLE){
        hr=pw_d3d9_service_texture_owners_drain(objects,ref);
        if(SUCCEEDED(hr))pw_d3d9_native_device_implicit_set_phase(d,PW_D3D9_IMPLICIT_DRAINED);
    }
    if(!pw_d3d9_object_complete(objects,ref))hr=E_FAIL;
    if(FAILED(hr)){memset(r,0,sizeof(*r));r->operation=q->operation;}
    else r->count=count;
    r->hresult=hr;return hr;
}
#endif
static HRESULT create_factory(struct pw_d3d9_objects *objects,IDirect3D9 *(WINAPI *factory)(UINT),
                              UINT sdk,struct pw_d3d9_object_ref *ref)
{
    IDirect3D9 *d3d=factory(sdk);IUnknown *identity=NULL;
    if(!d3d)return E_FAIL;
    HRESULT hr=IDirect3D9_QueryInterface(d3d,&IID_IUnknown,(void **)&identity);
    if(FAILED(hr)||!identity){IDirect3D9_Release(d3d);return E_NOINTERFACE;}
    if(pw_d3d9_object_find(objects,(uintptr_t)identity,ref)){
        hr=pw_d3d9_object_addref(objects,*ref,1)?S_OK:E_FAIL;
        IDirect3D9_Release(d3d);
    }else if(!pw_d3d9_object_reserve(objects,ref)){
        IDirect3D9_Release(d3d);hr=E_OUTOFMEMORY;
    }else if(!pw_d3d9_object_commit(objects,*ref,(uintptr_t)identity,(uintptr_t)d3d,1)){
        pw_d3d9_object_abort(objects,*ref);IDirect3D9_Release(d3d);hr=E_FAIL;
    }else hr=S_OK;
    IUnknown_Release(identity);return hr;
}
static void factory_call(IDirect3D9 *d3d,const struct pw_d3d9_factory_request *q,struct pw_d3d9_factory_reply *r)
{
    D3DADAPTER_IDENTIFIER9 identifier={0};D3DDISPLAYMODE mode={0};D3DCAPS9 caps={0};DWORD quality=0;
    memset(r,0,sizeof(*r));r->method=q->method;r->hresult=(uint32_t)E_NOTIMPL;
    switch(q->method){
    case 4:r->count=IDirect3D9_GetAdapterCount(d3d);r->hresult=0;break;
    case 5:
        r->hresult=IDirect3D9_GetAdapterIdentifier(d3d,q->adapter,q->flags,&identifier);
        if(FAILED((HRESULT)r->hresult))break;
        memcpy(r->identifier.driver,identifier.Driver,sizeof(identifier.Driver));
        memcpy(r->identifier.description,identifier.Description,sizeof(identifier.Description));
        memcpy(r->identifier.device_name,identifier.DeviceName,sizeof(identifier.DeviceName));
        r->identifier.driver_version=(uint64_t)identifier.DriverVersion.QuadPart;
        r->identifier.vendor_id=identifier.VendorId;r->identifier.device_id=identifier.DeviceId;
        r->identifier.subsystem_id=identifier.SubSysId;r->identifier.revision=identifier.Revision;
        r->identifier.guid_data1=identifier.DeviceIdentifier.Data1;r->identifier.guid_data2=identifier.DeviceIdentifier.Data2;
        r->identifier.guid_data3=identifier.DeviceIdentifier.Data3;memcpy(r->identifier.guid_data4,identifier.DeviceIdentifier.Data4,8);
        r->identifier.whql_level=identifier.WHQLLevel;break;
    case 6:r->count=IDirect3D9_GetAdapterModeCount(d3d,q->adapter,q->format);r->hresult=0;break;
    case 7:r->hresult=IDirect3D9_EnumAdapterModes(d3d,q->adapter,q->format,q->mode,&mode);break;
    case 8:r->hresult=IDirect3D9_GetAdapterDisplayMode(d3d,q->adapter,&mode);break;
    case 9:r->hresult=IDirect3D9_CheckDeviceType(d3d,q->adapter,q->device_type,q->format,q->format2,q->windowed);break;
    case 10:r->hresult=IDirect3D9_CheckDeviceFormat(d3d,q->adapter,q->device_type,q->format,q->usage,q->resource_type,q->format2);break;
    case 11:r->hresult=IDirect3D9_CheckDeviceMultiSampleType(d3d,q->adapter,q->device_type,q->format,q->windowed,q->multisample,&quality);r->count=quality;break;
    case 12:r->hresult=IDirect3D9_CheckDepthStencilMatch(d3d,q->adapter,q->device_type,q->format,q->format2,q->format3);break;
    case 13:r->hresult=IDirect3D9_CheckDeviceFormatConversion(d3d,q->adapter,q->device_type,q->format,q->format2);break;
    case 14:
        r->hresult=IDirect3D9_GetDeviceCaps(d3d,q->adapter,q->device_type,&caps);
        if(FAILED((HRESULT)r->hresult))break;
#define COPY_CAP(type,name,native) _Static_assert(sizeof(caps.native)==4,"native caps field width");memcpy(&r->caps.name,&caps.native,4);
        PW_D3D9_CAP_FIELDS(COPY_CAP)
#undef COPY_CAP
        break;
    }
    if((q->method==7||q->method==8) && SUCCEEDED((HRESULT)r->hresult))
        r->mode=(struct pw_d3d9_display_mode){mode.Width,mode.Height,mode.RefreshRate,mode.Format};
}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
    struct ipc ipc={0};struct descriptor desc;WCHAR object_name[96];DWORD error=1,pid=GetCurrentProcessId();
    uint32_t last_opcode=0;const char *phase="startup";
    int prof=profile_enabled(),profile_pending=0,profile_is_present=0;
    uint64_t dispatch_begin=0;struct pw_d3d9_message profile_message={0};
    HMODULE backend=NULL;IDirect3D9 *(WINAPI *factory)(UINT)=NULL;
    struct pw_d3d9_object_slot *slots=NULL;struct pw_d3d9_objects objects;BOOL objects_ready=FALSE;
    unsigned char scratch[RING_BYTES],output[RING_BYTES],hello[32];
#ifdef PW_D3D9_ENABLE_BATCH
    struct pw_d3d9_service_batch_state batches;pw_d3d9_service_batch_init(&batches);
#endif
    if(*(volatile LONG *)((char *)NtCurrentTeb()+0x180c))return 2;
    name(object_name,L"BOOT",pid,0);ipc.descriptor_mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,object_name);
    if(!ipc.descriptor_mapping)goto done;
    ipc.descriptor=MapViewOfFile(ipc.descriptor_mapping,FILE_MAP_READ,0,0,sizeof(desc));if(!ipc.descriptor)goto done;
    MemoryBarrier();memcpy(&desc,ipc.descriptor,sizeof(desc));
    if(desc.magic!=SESSION_MAGIC||desc.version!=1||desc.bytes!=sizeof(desc)||!desc.epoch||desc.pid!=pid||
       desc.ring_bytes!=RING_BYTES||desc.backend_crc!=BACKEND_CRC||desc.backend_bytes!=BACKEND_BYTES||
       desc.backend[259]||!absolute_path(desc.backend))goto done;
    name(object_name,L"WIRE",pid,desc.epoch);ipc.wire_mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,object_name);
    if(!ipc.wire_mapping)goto done;
    ipc.memory=MapViewOfFile(ipc.wire_mapping,FILE_MAP_ALL_ACCESS,0,0,pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES));
    if(!ipc.memory||pw_d3d9_channel_open(&ipc.channel,ipc.memory,pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES),desc.epoch,PW_D3D9_SERVICE)!=PW_D3D9_OK)goto done;
    const WCHAR *kinds[]={L"REQUEST",L"REPLY",L"OPEN",L"CANCEL"};
    HANDLE *events[]={&ipc.request,&ipc.reply,&ipc.opened,&ipc.cancel};
    for(unsigned n=0;n<4;n++){
        name(object_name,kinds[n],pid,desc.epoch);*events[n]=OpenEventW(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,object_name);if(!*events[n])goto done;
    }
    backend=load_backend(desc.backend);if(!backend)goto done;
    factory=(void *)GetProcAddress(backend,"Direct3DCreate9");if(!factory)goto done;
    slots=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,16384*sizeof(*slots));
    if(!slots||!pw_d3d9_objects_init(&objects,slots,16384,1,desc.epoch))goto done;
    objects_ready=TRUE;
    if(!SetEvent(ipc.opened))goto done;
    hello_payload(hello,desc.epoch);
    for(;;){
        struct pw_d3d9_message m;size_t bytes=0;BOOL stop=FALSE;
        phase="receive";
        if(receive_wait(&ipc,&m,scratch,sizeof(scratch),NULL)!=PW_D3D9_OK)goto done;
        last_opcode=m.opcode;phase="dispatch_or_encode";
#ifdef PW_D3D9_ENABLE_BATCH
        if(batches.failed_result)goto done;
#endif
        if(prof){profile_pending=1;profile_message=m;dispatch_begin=profile_now();
            profile_is_present=profile_present(&m,scratch+64);}
        const unsigned char *payload=scratch+64;HRESULT hr=S_OK;
        struct pw_d3d9_object_ref ref={m.object,m.generation};
        if(m.opcode==PW_D3D9_HELLO){
            if(m.device||m.object||m.payload_bytes!=32||memcmp(payload,hello,32)||pw_d3d9_channel_ready(&ipc.channel)!=PW_D3D9_OK)goto done;
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
            if(!pw_d3d9_service_batch_bindings(&batches,1))goto done;
#endif
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
            if(!pw_d3d9_service_batch_draws(&batches,1))goto done;
#endif
            memcpy(output,hello,32);bytes=32;
        }else if(m.opcode==PW_D3D9_STOP){
            if(m.device||m.object||m.payload_bytes)goto done;
#ifdef PW_D3D9_ENABLE_PROGRAM
            pw_d3d9_service_program_shutdown(&objects);
#endif
#ifdef PW_D3D9_ENABLE_UP
            pw_d3d9_service_up_shutdown(&objects);
#endif
            pw_d3d9_objects_cancel(&objects);
            if(!destroy_objects(&objects))goto done;
#ifdef PW_D3D9_ENABLE_DEVICE
            if(!pw_d3d9_native_device_shutdown())goto done;
#endif
            FreeLibrary(backend);backend=NULL;stop=TRUE;
        }else if(m.opcode==PW_D3D9_CREATE9){
            if(m.device!=1||m.object||m.payload_bytes!=4)goto done;
            hr=create_factory(&objects,factory,get32(payload),&ref);
            if(SUCCEEDED(hr)){put32(output,ref.id);put32(output+4,ref.generation);bytes=8;}
        }else if(m.opcode==PW_D3D9_RELEASE){
            if(m.device!=1||!m.object||m.payload_bytes)goto done;
            hr=pw_d3d9_object_release(&objects,ref)?S_OK:D3DERR_INVALIDCALL;
            if(!destroy_objects(&objects))goto done;
#ifdef PW_D3D9_ENABLE_BATCH
        }else if(m.opcode==PW_D3D9_COMMAND_BATCH_CALL){
            if(m.device!=objects.device||!pw_d3d9_service_batch(&batches,&objects,ref,payload,m.payload_bytes,
                output,sizeof(output),&bytes,&hr))goto done;
#endif
        }else if(m.opcode==PW_D3D9_FACTORY_CALL){
            struct pw_d3d9_factory_request request;struct pw_d3d9_factory_reply reply={0};
            if(pw_d3d9_factory_request_decode(&request,payload,m.payload_bytes)!=PW_D3D9_FACTORY_OK)goto done;
            reply.method=request.method;reply.hresult=D3DERR_INVALIDCALL;
            const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(&objects,m.device,desc.epoch,ref);
            if(slot&&slot->kind==1&&pw_d3d9_object_queue(&objects,ref)){
                factory_call((IDirect3D9 *)slot->context,&request,&reply);
                if(!pw_d3d9_object_complete(&objects,ref))goto done;
            }
            hr=(HRESULT)reply.hresult;
            pw_d3d9_factory_failure(stderr,&request,reply.hresult);
            /* Invalid targets carry only the outer failure; the typed count
             * codec deliberately cannot represent a failed count operation. */
            if(slot && slot->kind==1){
                if(pw_d3d9_factory_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_FACTORY_OK)goto done;
            }
#ifdef PW_D3D9_ENABLE_DEVICE
        }else if(m.opcode==PW_D3D9_DEVICE_CALL){
            struct pw_d3d9_device_request request;struct pw_d3d9_device_reply reply;
            struct pw_d3d9_native_device *created=NULL;
            if(pw_d3d9_device_request_decode(&request,payload,m.payload_bytes)!=PW_D3D9_DEVICE_OK)goto done;
            const struct pw_d3d9_object_slot *slot=pw_d3d9_object_lookup(&objects,m.device,desc.epoch,ref);
            unsigned kind=request.operation==PW_D3D9_DEVICE_CREATE?1:2;
            hr=D3DERR_INVALIDCALL;
            if(slot&&slot->kind==kind&&pw_d3d9_object_queue(&objects,ref)){
#ifdef PW_D3D9_ENABLE_IMPLICIT
                int resetting=request.operation==PW_D3D9_DEVICE_RESET;
                struct pw_d3d9_native_device *native=kind==2?(void *)slot->context:NULL;
                if(resetting && (pw_d3d9_native_device_implicit_phase(native)!=PW_D3D9_IMPLICIT_PREPARED ||
                    FAILED(pw_d3d9_service_texture_owners_begin_reset(&objects,ref)))){
                    pw_d3d9_object_complete(&objects,ref);goto done;
                }
#endif
                pw_d3d9_native_device_call(kind==1?(void *)slot->context:NULL,kind==2?(void *)slot->context:NULL,&request,&reply,&created);
#ifdef PW_D3D9_ENABLE_IMPLICIT
                if(resetting){
                    BOOL restore=pw_d3d9_native_device_reset_preserved(native);
                    if(FAILED(pw_d3d9_service_texture_owners_finish_reset(&objects,ref,restore))){
                        pw_d3d9_object_complete(&objects,ref);goto done;
                    }
                    pw_d3d9_native_device_implicit_set_phase(native,restore?PW_D3D9_IMPLICIT_DONE_RESTORED:PW_D3D9_IMPLICIT_DONE_RETIRED);
                    if(pw_d3d9_native_device_reset_succeeded(native) && FAILED(pw_d3d9_service_texture_owners_capture(&objects,ref,
                        reply.parameters.count,reply.parameters.auto_depth_stencil))){
                        pw_d3d9_object_complete(&objects,ref);goto done;
                    }
                }
#endif
                if(!pw_d3d9_object_complete(&objects,ref))goto done;
                if(created){
                    struct pw_d3d9_object_ref device_ref;
                    uintptr_t identity=pw_d3d9_native_device_identity(created);
                    if(!identity||!pw_d3d9_object_reserve(&objects,&device_ref)){
                        pw_d3d9_native_device_destroy(created);reply.hresult=E_OUTOFMEMORY;
                    }else if(!pw_d3d9_object_commit(&objects,device_ref,identity,(uintptr_t)created,2)){
                        pw_d3d9_object_abort(&objects,device_ref);pw_d3d9_native_device_destroy(created);reply.hresult=E_FAIL;
                    }else {
                        reply.object=device_ref;
#ifdef PW_D3D9_ENABLE_IMPLICIT
                        if(FAILED(pw_d3d9_service_texture_owners_capture(&objects,device_ref,
                            reply.parameters.count,reply.parameters.auto_depth_stencil)))goto done;
#endif
                    }
                }
                hr=(HRESULT)reply.hresult;
                if(pw_d3d9_device_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_DEVICE_OK)goto done;
            }
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
        }else if(m.opcode==PW_D3D9_RESOURCE_CALL){
            struct pw_d3d9_resource_request request;struct pw_d3d9_resource_reply reply;
            if(m.device!=objects.device||pw_d3d9_resource_request_decode(&request,payload,m.payload_bytes)!=PW_D3D9_RESOURCE_OK)goto done;
            pw_d3d9_service_resource_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_resource_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_RESOURCE_OK)goto done;
#endif
#ifdef PW_D3D9_ENABLE_TEXTURE
#ifdef PW_D3D9_ENABLE_IMPLICIT
        }else if(m.opcode==PW_D3D9_IMPLICIT_CALL){
            struct pw_d3d9_implicit_request request;struct pw_d3d9_implicit_reply reply;
            if(m.device!=objects.device||pw_d3d9_implicit_request_decode(&request,payload,m.payload_bytes))goto done;
            hr=implicit_call(&objects,ref,&request,&reply);
            if(pw_d3d9_implicit_reply_encode(output,PW_D3D9_IMPLICIT_REPLY_BYTES,&request,&reply))goto done;
            bytes=PW_D3D9_IMPLICIT_REPLY_BYTES;
            if(!destroy_objects(&objects))goto done;
#endif
        }else if(m.opcode==PW_D3D9_TEXTURE_CALL){
            struct pw_d3d9_texture_request request;struct pw_d3d9_texture_reply reply;
            if(m.device!=objects.device||pw_d3d9_texture_request_decode(&request,payload,m.payload_bytes)!=PW_D3D9_RESOURCE_OK)goto done;
            pw_d3d9_service_texture_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_texture_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_RESOURCE_OK)goto done;
#endif
#ifdef PW_D3D9_ENABLE_METHODS
        }else if(m.opcode==PW_D3D9_COMMAND_CALL||m.opcode==PW_D3D9_GETTER_CALL
#ifdef PW_D3D9_ENABLE_CURSOR
                  ||m.opcode==PW_D3D9_CURSOR_CALL
#endif
#ifdef PW_D3D9_ENABLE_GAMMA
                  ||m.opcode==PW_D3D9_GAMMA_CALL
#endif
){
            if(m.device!=objects.device||!pw_d3d9_service_methods(&objects,ref,m.opcode,payload,m.payload_bytes,output,sizeof(output),&bytes,&hr))goto done;
#endif
#ifdef PW_D3D9_ENABLE_PROGRAM
        }else if(m.opcode==PW_D3D9_PROGRAM_QUERY_CALL){
            struct pw_d3d9_program_query_request request;struct pw_d3d9_program_query_reply reply;
            if(m.device!=objects.device||pw_d3d9_program_query_decode(&request,payload,m.payload_bytes)!=PW_D3D9_PROGRAM_OK)goto done;
            pw_d3d9_service_program_query(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_program_query_reply_encode(output,sizeof(output),&bytes,&request,&reply)!=PW_D3D9_PROGRAM_OK)goto done;
        }else if(m.opcode==PW_D3D9_PROGRAM_CALL){
            struct pw_d3d9_program_request request;struct pw_d3d9_program_reply reply;
            if(m.device!=objects.device||pw_d3d9_program_decode(&request,payload,m.payload_bytes)!=PW_D3D9_PROGRAM_OK)goto done;
            pw_d3d9_service_program_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_program_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_PROGRAM_OK)goto done;
#endif
#ifdef PW_D3D9_ENABLE_UP
        }else if(m.opcode==PW_D3D9_UP_DRAW_CALL){
            struct pw_d3d9_up_request request;struct pw_d3d9_up_reply reply;
            if(m.device!=objects.device||pw_d3d9_up_decode(&request,payload,m.payload_bytes)!=PW_D3D9_UP_OK)goto done;
            pw_d3d9_service_up_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_up_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_UP_OK)goto done;
#endif
#ifdef PW_D3D9_ENABLE_OBJECT_GETTER
        }else if(m.opcode==PW_D3D9_OBJECT_GETTER_CALL){
            struct pw_d3d9_object_getter_request request;struct pw_d3d9_object_getter_reply reply;
            if(m.device!=objects.device||pw_d3d9_object_getter_decode(&request,payload,m.payload_bytes)!=PW_D3D9_OBJECT_GETTER_OK)goto done;
            pw_d3d9_service_object_getter(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_object_getter_reply_encode(output,sizeof(output),&bytes,&request,&reply)!=PW_D3D9_OBJECT_GETTER_OK)goto done;
#endif
#ifdef PW_D3D9_ENABLE_QUERY
        }else if(m.opcode==PW_D3D9_QUERY_CALL){
            struct pw_d3d9_query_request request;struct pw_d3d9_query_reply reply;
            if(m.device!=objects.device||pw_d3d9_query_request_decode(&request,payload,m.payload_bytes))goto done;
            pw_d3d9_service_query_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_query_reply_encode(output,sizeof(output),&bytes,&request,&reply))goto done;
#endif
#ifdef PW_D3D9_ENABLE_STATEBLOCK
        }else if(m.opcode==PW_D3D9_STATEBLOCK_CALL){
            struct pw_d3d9_stateblock_request request;struct pw_d3d9_stateblock_reply reply;
            if(m.device!=objects.device||pw_d3d9_stateblock_decode(&request,payload,m.payload_bytes)!=PW_D3D9_SB_OK)goto done;
            pw_d3d9_service_stateblock_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_service_stateblock_reply(output,sizeof(output),&bytes,&request,&reply)!=PW_D3D9_SB_OK)goto done;
#endif
        }else goto done;
        m.sequence=0;m.result=hr;m.payload_bytes=(uint32_t)bytes;
        phase="send_reply";
        int reply_sent=send_wake(&ipc,&m,output);
        if(prof){struct pw_d3d9_transport_stats sample={0};struct profile_record record;
            sample.attempts=sample.published=1;sample.request_bytes=64u+profile_message.payload_bytes;
            if(profile_message.opcode<PW_D3D9_STATS_OPS)sample.opcode[profile_message.opcode]=1;
            sample.replies=reply_sent==PW_D3D9_OK;
#ifdef PW_D3D9_ENABLE_BATCH
            if(profile_message.opcode==PW_D3D9_COMMAND_BATCH_CALL){sample.batch_flushes=1;sample.batch_commands=get32(payload+4);}
#endif
            sample.reply_bytes=sample.replies?64u+bytes:0;sample.failures=FAILED(hr)||!sample.replies;
            sample.service_dispatch_wall_us=pw_d3d9_stats_elapsed(dispatch_begin,profile_now(),&sample.clock_invalid);
            profile_capture(&ipc,&sample,&profile_message,profile_message.sequence,reply_sent==PW_D3D9_OK?hr:E_FAIL,profile_is_present,(int)sample.replies,0,&record);
            profile_pending=0;profile_emit(&record);}
        if(reply_sent!=PW_D3D9_OK)goto done;
        result[0]++;
#ifdef PW_D3D9_ENABLE_BATCH
        if(batches.failed_result){
            /* Keep the exact prefix failure readable. A later request already
             * in the ring must never trigger cancellation ahead of this ACK. */
            phase="failure_ack_wait";
            pw_d3d9_failure_wait(ipc.cancel);
            goto done;
        }
#endif
        if(stop){if(pw_d3d9_channel_stopped(&ipc.channel)!=PW_D3D9_OK)goto done;error=0;break;}
    }
 done:
    if(prof&&profile_pending){struct pw_d3d9_transport_stats sample={0};struct profile_record record;
        sample.attempts=sample.published=sample.failures=1;sample.request_bytes=64u+profile_message.payload_bytes;
        if(profile_message.opcode<PW_D3D9_STATS_OPS)sample.opcode[profile_message.opcode]=1;
        sample.service_dispatch_wall_us=pw_d3d9_stats_elapsed(dispatch_begin,profile_now(),&sample.clock_invalid);
        profile_capture(&ipc,&sample,&profile_message,profile_message.sequence,E_FAIL,profile_is_present,0,0,&record);profile_emit(&record);}
    if(error){fprintf(stderr,"PW_D3D9 service opcode=%u phase=%s error=%lu\n",last_opcode,phase,error);cancel_ipc(&ipc);}
    if(objects_ready){
#ifdef PW_D3D9_ENABLE_PROGRAM
        pw_d3d9_service_program_shutdown(&objects);
#endif
#ifdef PW_D3D9_ENABLE_UP
        pw_d3d9_service_up_shutdown(&objects);
#endif
        pw_d3d9_objects_cancel(&objects);destroy_objects(&objects);
    }
#ifdef PW_D3D9_ENABLE_DEVICE
    if(!pw_d3d9_native_device_shutdown())error=3;
#endif
    if(backend)FreeLibrary(backend);
    if(slots)HeapFree(GetProcessHeap(),0,slots);
    close_ipc(&ipc);return error;
}
#endif
