/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#ifdef PW_D3D9_ENABLE_TEXTURE
#include "pw_d3d9_service_texture.h"
#endif
#include "pw_d3d9_session.h"
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
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define SESSION_MAGIC 0x39535750u
#define RING_BYTES 8192u
#define BACKEND_BYTES 3919872u
#define BACKEND_CRC 0x6d86db72u
#define FACTORY_METHODS 0x00007ff0u /* slots 4 through 14 */
#define TRANSPORT_ERROR 0x100u
#ifdef PW_D3D9_ENABLE_DEVICE
#define DEVICE_FEATURES 1u
#else
#define DEVICE_FEATURES 0u
#endif
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
};
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
    uint32_t fields[]={1,epoch,RING_BYTES,BACKEND_CRC,BACKEND_BYTES,FACTORY_METHODS,DEVICE_FEATURES,0};
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
    for(;;){
        int status=pw_d3d9_channel_receive(&i->channel,m,scratch,bytes);
        if(status!=PW_D3D9_EMPTY)return status;
        DWORD wait;
#if defined(_WIN64) && defined(PW_D3D9_ENABLE_DEVICE)
        wait=MsgWaitForMultipleObjects(2,waits,FALSE,INFINITE,QS_ALLINPUT);
        if(wait==WAIT_OBJECT_0+2){MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}continue;}
#elif defined(_WIN64)
        wait=WaitForMultipleObjects(peer?3:2,waits,FALSE,peer?30000:INFINITE);
#else
        wait=client_wait(peer?3:2,waits,peer?30000:INFINITE);
#endif
        if(wait!=WAIT_OBJECT_0){cancel_ipc(i);return PW_D3D9_CLOSED;}
    }
}
static int send_wake(struct ipc *i,struct pw_d3d9_message *m,const void *payload)
{
    int status=pw_d3d9_channel_send(&i->channel,m,payload);
    if(status==PW_D3D9_OK && !SetEvent(i->channel.role==PW_D3D9_CLIENT?i->request:i->reply)){
        cancel_ipc(i);return PW_D3D9_CLOSED;
    }
    return status;
}

#ifndef _WIN64
struct bootstrap { ULONG version,size;WCHAR path[260];uint64_t result[8]; };
struct pw_d3d9_session {
    struct ipc ipc;
    HANDLE broker,serial_event;
    DWORD active_thread;
    CRITICAL_SECTION lock;
    SRWLOCK deferred_lock;
    struct pw_d3d9_deferred *deferred_first,*deferred_last;
    int draining;
    struct bootstrap bootstrap;
    LONG status;
};
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
HRESULT pw_d3d9_session_defer(struct pw_d3d9_session *s,struct pw_d3d9_deferred *item)
{
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
static HRESULT transact(struct pw_d3d9_session *s,struct pw_d3d9_message *m,const void *payload,
                        unsigned char *output,size_t capacity,struct pw_d3d9_message *reply)
{
    HRESULT result=E_FAIL;unsigned char scratch[RING_BYTES];const char *phase="send";
    memset(reply,0,sizeof(*reply));
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    while(!TryEnterCriticalSection(&s->lock)){
        DWORD wait;
#ifdef PW_D3D9_SESSION_TEST_CALLBACK
        extern int pw_d3d9_session_test_serial_failure(void);
        if(pw_d3d9_session_test_serial_failure()){client_pump();wait=WAIT_FAILED;}
        else
#endif
        wait=client_wait(1,&s->serial_event,30000);
        if(wait!=WAIT_OBJECT_0){restore_quit();drain_deferred(s);return E_FAIL;}
    }
    if(s->active_thread){LeaveCriticalSection(&s->lock);return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;}
    s->active_thread=GetCurrentThreadId();
    if(send_wake(&s->ipc,m,payload)!=PW_D3D9_OK)goto done;
#ifdef PW_D3D9_SESSION_TEST_CALLBACK
    extern void pw_d3d9_session_test_callback(void);
    pw_d3d9_session_test_callback();
#endif
    client_pump();
    phase="receive";
    if(receive_wait(&s->ipc,reply,scratch,sizeof(scratch),s->broker)!=PW_D3D9_OK)goto done;
    phase="reply_capacity";
    if(reply->payload_bytes>capacity){cancel_ipc(&s->ipc);goto done;}
    if(reply->payload_bytes)memcpy(output,scratch+64,reply->payload_bytes);
    phase="backend_result";result=(HRESULT)reply->result;
 done:
    if(FAILED(result))fprintf(stderr,"PW_D3D9 transaction opcode=%u phase=%s hr=%08lx reply_bytes=%u\n",m->opcode,phase,(DWORD)result,reply->payload_bytes);
    s->active_thread=0;LeaveCriticalSection(&s->lock);SetEvent(s->serial_event);
    restore_quit();drain_deferred(s);return result;
}
static void destroy_session(struct pw_d3d9_session *s)
{
    if(s->broker){client_wait(1,&s->broker,INFINITE);CloseHandle(s->broker);}
    if(s->serial_event)CloseHandle(s->serial_event);
    restore_quit();close_ipc(&s->ipc);DeleteCriticalSection(&s->lock);HeapFree(GetProcessHeap(),0,s);
    InterlockedExchange(&session_claim,0);
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
HRESULT pw_d3d9_session_create(struct pw_d3d9_session *s,UINT sdk,struct pw_d3d9_object_ref *ref)
{
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
#endif
#ifdef PW_D3D9_ENABLE_RESOURCE
HRESULT pw_d3d9_session_resource(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,
 const struct pw_d3d9_resource_request *request,struct pw_d3d9_resource_reply *reply)
{
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
    unsigned char in[RING_BYTES],out[16];size_t bytes;uint32_t method,hresult;
    struct pw_d3d9_message m={.opcode=PW_D3D9_COMMAND_CALL,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s||!request)return E_POINTER;
    if(pw_d3d9_command_encode(in,sizeof(in),&bytes,request)!=PW_D3D9_COMMAND_OK)return D3DERR_INVALIDCALL;
    m.payload_bytes=(uint32_t)bytes;HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_command_reply_decode(&method,&hresult,out,r.payload_bytes)!=PW_D3D9_COMMAND_OK||method!=request->method||hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    return hr;
}
HRESULT pw_d3d9_session_getter(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_getter_request *request,struct pw_d3d9_getter_reply *reply)
{
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
#ifdef PW_D3D9_ENABLE_STATEBLOCK
HRESULT pw_d3d9_session_stateblock(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref,const struct pw_d3d9_stateblock_request *request,struct pw_d3d9_stateblock_reply *reply)
{
    unsigned char in[16],out[16];struct pw_d3d9_stateblock_reply decoded;
    struct pw_d3d9_message m={.opcode=PW_D3D9_STATEBLOCK_CALL,.device=1,.object=ref.id,.generation=ref.generation,.payload_bytes=16},r;
    if(!s||!request||!reply)return E_POINTER;
    if(pw_d3d9_stateblock_encode(in,sizeof(in),request)!=PW_D3D9_SB_OK)return D3DERR_INVALIDCALL;
    HRESULT hr=transact(s,&m,in,out,sizeof(out),&r);
    if(FAILED(hr)&&!r.payload_bytes)return hr;
    if(pw_d3d9_stateblock_reply_decode(&decoded,request,out,r.payload_bytes)!=PW_D3D9_SB_OK||decoded.hresult!=(uint32_t)hr){cancel_ipc(&s->ipc);return E_FAIL;}
    *reply=decoded;return hr;
}
#endif
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *s,struct pw_d3d9_object_ref ref)
{
    struct pw_d3d9_message m={.opcode=PW_D3D9_RELEASE,.device=1,.object=ref.id,.generation=ref.generation},r;
    if(!s)return E_POINTER;
    return transact(s,&m,NULL,NULL,0,&r);
}
void pw_d3d9_session_cancel(struct pw_d3d9_session *s)
{ if(s)cancel_ipc(&s->ipc); }
HRESULT pw_d3d9_session_join(struct pw_d3d9_session *s)
{
    if(!s)return E_POINTER;
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    DWORD wait=client_wait(1,&s->broker,INFINITE);restore_quit();
    return wait==WAIT_OBJECT_0&&!s->status?S_OK:E_FAIL;
}
HRESULT pw_d3d9_session_close(struct pw_d3d9_session *s)
{
    struct pw_d3d9_message m={.opcode=PW_D3D9_STOP},r;HRESULT hr=E_FAIL;
    if(!s)return E_POINTER;
    if(callback_depth())return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
    if(pw_d3d9_channel_stop(&s->ipc.channel)==PW_D3D9_OK)hr=transact(s,&m,NULL,NULL,0,&r);
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
#ifdef PW_D3D9_ENABLE_STATEBLOCK
            else if(objects->slots[n].kind==10){if(FAILED(pw_d3d9_service_stateblock_destroy(objects,context)))okay=0;}
#endif
#ifdef PW_D3D9_ENABLE_DEVICE
            else if(objects->slots[n].kind==2){
#ifdef PW_D3D9_ENABLE_PROGRAM
                pw_d3d9_service_program_retire(objects,ref);
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
    HMODULE backend=NULL;IDirect3D9 *(WINAPI *factory)(UINT)=NULL;
    struct pw_d3d9_object_slot *slots=NULL;struct pw_d3d9_objects objects;BOOL objects_ready=FALSE;
    unsigned char scratch[RING_BYTES],output[RING_BYTES],hello[32];
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
        const unsigned char *payload=scratch+64;HRESULT hr=S_OK;
        struct pw_d3d9_object_ref ref={m.object,m.generation};
        if(m.opcode==PW_D3D9_HELLO){
            if(m.device||m.object||m.payload_bytes!=32||memcmp(payload,hello,32)||pw_d3d9_channel_ready(&ipc.channel)!=PW_D3D9_OK)goto done;
            memcpy(output,hello,32);bytes=32;
        }else if(m.opcode==PW_D3D9_STOP){
            if(m.device||m.object||m.payload_bytes)goto done;
#ifdef PW_D3D9_ENABLE_PROGRAM
            pw_d3d9_service_program_shutdown(&objects);
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
                pw_d3d9_native_device_call(kind==1?(void *)slot->context:NULL,kind==2?(void *)slot->context:NULL,&request,&reply,&created);
                if(!pw_d3d9_object_complete(&objects,ref))goto done;
                if(created){
                    struct pw_d3d9_object_ref device_ref;
                    uintptr_t identity=pw_d3d9_native_device_identity(created);
                    if(!identity||!pw_d3d9_object_reserve(&objects,&device_ref)){
                        pw_d3d9_native_device_destroy(created);reply.hresult=E_OUTOFMEMORY;
                    }else if(!pw_d3d9_object_commit(&objects,device_ref,identity,(uintptr_t)created,2)){
                        pw_d3d9_object_abort(&objects,device_ref);pw_d3d9_native_device_destroy(created);reply.hresult=E_FAIL;
                    }else reply.object=device_ref;
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
        }else if(m.opcode==PW_D3D9_TEXTURE_CALL){
            struct pw_d3d9_texture_request request;struct pw_d3d9_texture_reply reply;
            if(m.device!=objects.device||pw_d3d9_texture_request_decode(&request,payload,m.payload_bytes)!=PW_D3D9_RESOURCE_OK)goto done;
            pw_d3d9_service_texture_call(&objects,ref,&request,&reply);hr=(HRESULT)reply.hresult;
            if(pw_d3d9_texture_reply_encode(output,sizeof(output),&bytes,&reply)!=PW_D3D9_RESOURCE_OK)goto done;
#endif
#ifdef PW_D3D9_ENABLE_METHODS
        }else if(m.opcode==PW_D3D9_COMMAND_CALL||m.opcode==PW_D3D9_GETTER_CALL){
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
        if(send_wake(&ipc,&m,output)!=PW_D3D9_OK)goto done;
        result[0]++;
        if(stop){if(pw_d3d9_channel_stopped(&ipc.channel)!=PW_D3D9_OK)goto done;error=0;break;}
    }
 done:
    if(error){fprintf(stderr,"PW_D3D9 service opcode=%u phase=%s error=%lu\n",last_opcode,phase,error);cancel_ipc(&ipc);}
    if(objects_ready){
#ifdef PW_D3D9_ENABLE_PROGRAM
        pw_d3d9_service_program_shutdown(&objects);
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
