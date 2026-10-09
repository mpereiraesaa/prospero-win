/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* PE32 synchronous transport round trip: the real transact()/receive_wait()/
 * send_wake() code against an in-process responder on a second thread. Counts
 * the Win32 calls that reach wineserver and times ns per call. */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
static volatile LONG set_events,peeks,queue_polls,waits;
static BOOL WINAPI counted_set_event(HANDLE h){InterlockedIncrement(&set_events);return SetEvent(h);}
static BOOL WINAPI counted_peek(MSG *m,HWND w,UINT a,UINT b,UINT f){InterlockedIncrement(&peeks);return PeekMessageW(m,w,a,b,f);}
/* Stand-in for a callback that Wine event processing runs inside
 * GetQueueStatus: it must observe the guard and be refused re-entry. */
struct pw_d3d9_session;
static unsigned callback_depth(void);
static HRESULT queue_status_reentry(void);
static volatile LONG guarded_polls,unguarded_polls,reentry_refused;
__attribute__((unused)) static DWORD WINAPI counted_queue_status(UINT f)
{
    InterlockedIncrement(&queue_polls);
    if(!callback_depth())InterlockedIncrement(&unguarded_polls);
    else{InterlockedIncrement(&guarded_polls);if(queue_status_reentry()==RPC_E_CANTCALLOUT_ININPUTSYNCCALL)InterlockedIncrement(&reentry_refused);}
    return GetQueueStatus(f);
}
static DWORD WINAPI counted_msg_wait(DWORD n,const HANDLE *h,BOOL all,DWORD t,DWORD mask)
{InterlockedIncrement(&waits);return MsgWaitForMultipleObjects(n,h,all,t,mask);}
#define SetEvent counted_set_event
#define PeekMessageW counted_peek
#define GetQueueStatus counted_queue_status
#define MsgWaitForMultipleObjects counted_msg_wait
#include "../../wine/ps5/d3d9/pw_d3d9_session.c"
#undef SetEvent
#undef PeekMessageW
#undef GetQueueStatus
#undef MsgWaitForMultipleObjects
#define RING 8192u
static struct pw_d3d9_session *reentry_session;
static HRESULT queue_status_reentry(void)
{
    struct pw_d3d9_message reply,q={.opcode=PW_D3D9_FACTORY_CALL,.device=1,.object=1,.generation=1};
    return reentry_session?transact(reentry_session,&q,NULL,NULL,0,&reply):RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
}
#define PAYLOAD 32u
enum { MODE_FAST, MODE_SLOW_SERVICE, MODE_SLOW_CLIENT, MODE_CONTENDED, MODE_CANCEL };
#define CALLERS 4u
static int mode;
static volatile LONG responder_ready;
static void busy_us(unsigned us)
{
    LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
    do QueryPerformanceCounter(&b);while((uint64_t)(b.QuadPart-a.QuadPart)*1000000u/(uint64_t)f.QuadPart<us);
}
static DWORD WINAPI responder(void *arg)
{
    struct ipc *client=arg,peer={0};struct pw_d3d9_message m;unsigned char scratch[RING];
    peer.memory=client->memory;peer.request=client->request;peer.reply=client->reply;peer.cancel=client->cancel;
    assert(pw_d3d9_channel_open(&peer.channel,peer.memory,pw_d3d9_channel_bytes(RING,RING),77,PW_D3D9_SERVICE)==PW_D3D9_OK);
    InterlockedExchange(&responder_ready,1);
    for(unsigned n=0;;n++){
        if(receive_wait(&peer,&m,scratch,sizeof(scratch),NULL)!=PW_D3D9_OK)return 0;
        if(!n)assert(pw_d3d9_channel_ready(&peer.channel)==PW_D3D9_OK);
        if(mode==MODE_CANCEL&&n)continue; /* never answer: the client must be woken by cancel */
        if(mode==MODE_SLOW_SERVICE&&n%8==0)busy_us(200);
        m.sequence=0;m.result=S_OK;
        assert(send_wake(&peer,&m,scratch+64)==PW_D3D9_OK);
        if(m.opcode==PW_D3D9_STOP)return 0;
    }
}
struct caller { struct pw_d3d9_session *s; unsigned calls, id; };
static void call_loop(struct pw_d3d9_session *s,unsigned calls,unsigned id)
{
    struct pw_d3d9_message reply,q={.opcode=PW_D3D9_FACTORY_CALL,.device=1,.object=1,.generation=1,.payload_bytes=PAYLOAD};
    unsigned char in[PAYLOAD],out[PAYLOAD];
    for(unsigned n=0;n<calls;n++){
        memset(in,(int)(n*7+id+1),sizeof(in));
        HRESULT hr=transact(s,&q,in,out,sizeof(out),&reply);
        assert(hr==S_OK&&reply.payload_bytes==PAYLOAD&&!memcmp(in,out,PAYLOAD));
        if(mode==MODE_SLOW_CLIENT&&n%8==0)busy_us(200);
    }
}
static DWORD WINAPI caller_main(void *arg)
{struct caller *c=arg;call_loop(c->s,c->calls,c->id);return 0;}
static DWORD WINAPI canceller(void *arg)
{Sleep(100);cancel_ipc(arg);return 0;}
int main(int argc,char **argv)
{
    unsigned calls=argc>2?(unsigned)atoi(argv[2]):20000;LARGE_INTEGER f,a,b;
    if(argc<2)return 2;
    mode=!strcmp(argv[1],"fast")?MODE_FAST:!strcmp(argv[1],"slow-service")?MODE_SLOW_SERVICE:
         !strcmp(argv[1],"slow-client")?MODE_SLOW_CLIENT:!strcmp(argv[1],"contended")?MODE_CONTENDED:!strcmp(argv[1],"cancel")?MODE_CANCEL:-1;
    if(mode<0||!calls)return 2;
    assert(InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL));
    struct pw_d3d9_session s={0};InitializeCriticalSection(&s.lock);InitializeSRWLock(&s.deferred_lock);reentry_session=&s;
    s.serial_event=CreateEventW(NULL,FALSE,FALSE,NULL);
    s.ipc.request=CreateEventW(NULL,FALSE,FALSE,NULL);s.ipc.reply=CreateEventW(NULL,FALSE,FALSE,NULL);
    s.ipc.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
    size_t bytes=pw_d3d9_channel_bytes(RING,RING);s.ipc.memory=VirtualAlloc(NULL,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    assert(s.ipc.memory&&pw_d3d9_channel_init(s.ipc.memory,bytes,77,RING,RING)==PW_D3D9_OK);
    assert(pw_d3d9_channel_open(&s.ipc.channel,s.ipc.memory,bytes,77,PW_D3D9_CLIENT)==PW_D3D9_OK);
    HANDLE thread=CreateThread(NULL,0,responder,&s.ipc,0,NULL);assert(thread);
    while(!responder_ready)Sleep(1);
    s.broker=thread; /* peer-death handle, as the broker is in production */
    struct pw_d3d9_message reply,q={.opcode=PW_D3D9_HELLO};unsigned char in[PAYLOAD],out[PAYLOAD];
    assert(transact(&s,&q,NULL,NULL,0,&reply)==S_OK);
#ifdef PW_D3D9_SPIN_POLLS
    {   /* Cost of one exhausted receive spin on an empty ring. */
        unsigned char scratch[RING];QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
        for(unsigned rep=0;rep<100;rep++)for(unsigned n=0;n<PW_D3D9_SPIN_POLLS;n++){
            assert(pw_d3d9_channel_receive(&s.ipc.channel,&reply,scratch,sizeof(scratch))==PW_D3D9_EMPTY);YieldProcessor();}
        QueryPerformanceCounter(&b);
        printf("PW_TRANSPORT_ROUNDTRIP spin_polls=%u spin_ns=%llu\n",PW_D3D9_SPIN_POLLS,
               (unsigned long long)((uint64_t)(b.QuadPart-a.QuadPart)*10000000u/(uint64_t)f.QuadPart));
    }
#endif
    if(mode==MODE_CANCEL){
        HANDLE c=CreateThread(NULL,0,canceller,&s.ipc,0,NULL);assert(c);
        q=(struct pw_d3d9_message){.opcode=PW_D3D9_FACTORY_CALL,.device=1,.object=1,.generation=1,.payload_bytes=PAYLOAD};
        memset(in,0x5a,sizeof(in));QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
        HRESULT hr=transact(&s,&q,in,out,sizeof(out),&reply);QueryPerformanceCounter(&b);
        uint64_t ms=(uint64_t)(b.QuadPart-a.QuadPart)*1000u/(uint64_t)f.QuadPart;
        assert(FAILED(hr)&&ms<5000);
        assert(WaitForSingleObject(c,5000)==WAIT_OBJECT_0&&WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
        printf("PW_TRANSPORT_ROUNDTRIP mode=cancel hr=%08lx woke_ms=%llu\n",(DWORD)hr,(unsigned long long)ms);return 0;
    }
    q=(struct pw_d3d9_message){.opcode=PW_D3D9_FACTORY_CALL,.device=1,.object=1,.generation=1,.payload_bytes=PAYLOAD};
    for(unsigned n=0;n<1000;n++){memset(in,n,sizeof(in));assert(transact(&s,&q,in,out,sizeof(out),&reply)==S_OK);}
    LONG e0=set_events,p0=peeks,g0=queue_polls,w0=waits;
    QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
    if(mode==MODE_CONTENDED){
        /* Concurrent callers serialize on the session lock and its waiter count. */
        struct caller c[CALLERS];HANDLE t[CALLERS];
        for(unsigned n=0;n<CALLERS;n++){c[n]=(struct caller){&s,calls/CALLERS,n};t[n]=CreateThread(NULL,0,caller_main,&c[n],0,NULL);assert(t[n]);}
        assert(WaitForMultipleObjects(CALLERS,t,TRUE,120000)==WAIT_OBJECT_0);
        for(unsigned n=0;n<CALLERS;n++)CloseHandle(t[n]);
        calls=calls/CALLERS*CALLERS;
    }else call_loop(&s,calls,0);
    QueryPerformanceCounter(&b);
    uint64_t ns=(uint64_t)(b.QuadPart-a.QuadPart)*1000000000u/(uint64_t)f.QuadPart;
    /* Every queue poll ran under the callback guard and refused re-entry. */
    assert(queue_polls&&!unguarded_polls&&guarded_polls==queue_polls&&reentry_refused==guarded_polls);
    printf("PW_TRANSPORT_ROUNDTRIP queue_status_guarded=%ld reentry_refused=%ld unguarded=%ld\n",guarded_polls,reentry_refused,unguarded_polls);
    printf("PW_TRANSPORT_ROUNDTRIP mode=%s calls=%u ns_per_call=%llu set_event=%.3f peek=%.3f queue_status=%.3f msg_wait=%.3f per_call\n",
           argv[1],calls,(unsigned long long)(ns/calls),(double)(set_events-e0)/calls,(double)(peeks-p0)/calls,
           (double)(queue_polls-g0)/calls,(double)(waits-w0)/calls);
    assert(pw_d3d9_channel_stop(&s.ipc.channel)==PW_D3D9_OK);q=(struct pw_d3d9_message){.opcode=PW_D3D9_STOP};
    assert(transact(&s,&q,NULL,NULL,0,&reply)==S_OK);
    assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    return 0;
}
