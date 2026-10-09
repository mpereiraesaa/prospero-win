/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <assert.h>
static void *watched_free;static LONG free_count;
static BOOL WINAPI tracked_free(HANDLE heap,DWORD flags,LPVOID pointer)
{if(pointer==watched_free)InterlockedIncrement(&free_count);return HeapFree(heap,flags,pointer);}
#define HeapFree tracked_free
#define PW_D3D9_ENABLE_BINDING_TICKETS
#define PW_D3D9_BINDING_FEATURE 32768u
#define PW_D3D9_SESSION_TEST_CALLBACK
static int cancel_on_callback,mapped_wire;

static ULONGLONG ticks=1;
static int timed_clock;static LONGLONG clock_ticks;
static BOOL WINAPI test_counter(LARGE_INTEGER *value)
{if(!timed_clock)return QueryPerformanceCounter(value);clock_ticks+=100;value->QuadPart=clock_ticks;return TRUE;}
#define QueryPerformanceCounter test_counter
static ULONGLONG WINAPI test_ticks(void){return ticks;}
#define GetTickCount64 test_ticks
#include "../../wine/ps5/pw_d3d9_bridge_wire.h"
static int receive_failure;
static int test_receive(struct pw_d3d9_channel *,struct pw_d3d9_message *,void *,size_t);
#define pw_d3d9_channel_receive test_receive
#define PW_D3D9_ENABLE_METHODS
#define PW_D3D9_ENABLE_BATCH
#include "../../wine/ps5/d3d9/pw_d3d9_session.c"
#undef GetTickCount64
#undef QueryPerformanceCounter
#undef HeapFree
#undef pw_d3d9_channel_receive
static int test_receive(struct pw_d3d9_channel *c,struct pw_d3d9_message *m,void *buffer,size_t size)
{int status=pw_d3d9_channel_receive(c,m,buffer,size);if(receive_failure&&c->role==PW_D3D9_CLIENT&&status==PW_D3D9_OK&&m->opcode==PW_D3D9_COMMAND_CALL){receive_failure=0;assert(m->payload_bytes==16);return PW_D3D9_INVALID;}return status;}
int pw_d3d9_session_test_serial_failure(void){return 0;}
void pw_d3d9_session_test_callback(void);
struct fixture {struct pw_d3d9_session session;HANDLE thread,enter,arrived;LONG requests,batches,commands;UINT values[1024];int fail;};
static DWORD WINAPI peer(void *arg)
{
 struct fixture *f=arg;struct ipc p={0};struct pw_d3d9_message m;unsigned char scratch[RING_BYTES],out[32];uint64_t next=1;
 p.memory=f->session.ipc.memory;p.request=f->session.ipc.request;p.reply=f->session.ipc.reply;p.cancel=f->session.ipc.cancel;
 assert(pw_d3d9_channel_open(&p.channel,p.memory,pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES),1,PW_D3D9_SERVICE)==PW_D3D9_OK);SetEvent(f->session.ipc.opened);
 for(;;){
  if(receive_wait(&p,&m,scratch,sizeof(scratch),NULL)!=PW_D3D9_OK)break;
  InterlockedIncrement(&f->requests);unsigned bytes=0;HRESULT hr=S_OK;int stop=m.opcode==PW_D3D9_STOP;
  if(m.opcode==PW_D3D9_HELLO)assert(pw_d3d9_channel_ready(&p.channel)==PW_D3D9_OK);
  if(m.opcode==PW_D3D9_COMMAND_BATCH_CALL){
   struct pw_d3d9_command_batch b;assert(!pw_d3d9_batch_decode(&b,scratch+64,m.payload_bytes)&&b.first_sequence==next);
   struct pw_d3d9_batch_reply r={next,b.count,0,UINT32_MAX,0};InterlockedIncrement(&f->batches);
   for(unsigned n=0;n<b.count;n++){
    struct pw_d3d9_command q;struct pw_d3d9_binding binding;assert(!pw_d3d9_batch_command(&q,&b,n)&&(pw_d3d9_command_can_queue(&q)||pw_d3d9_binding_plan(&q,&binding)==PW_D3D9_BINDING_READY));
    f->values[f->commands++]=q.args[1];r.attempted++;next++;
    if(f->fail&&n==1){r.failed_index=n;r.hresult=hr=D3DERR_INVALIDCALL;break;}
   }
   assert(!pw_d3d9_batch_reply_encode(out,sizeof(out),&r));bytes=sizeof(out);
  }
  if(m.opcode==PW_D3D9_COMMAND_CALL){
   struct pw_d3d9_command q;size_t written;
   assert(!pw_d3d9_command_decode(&q,scratch+64,m.payload_bytes));
   f->values[f->commands++]=q.args[1];
   assert(!pw_d3d9_command_reply_encode(out,sizeof(out),&written,(f->fail==2?57:q.method),S_OK));bytes=(unsigned)written;
  }
  if(f->fail>=2&&m.opcode==PW_D3D9_COMMAND_CALL){
   SetEvent(f->enter);assert(WaitForSingleObject(f->arrived,1000)==WAIT_OBJECT_0);
   DWORD start=GetTickCount();while(!InterlockedCompareExchange(&f->session.serial_waiters,0,0)&&GetTickCount()-start<1000)Sleep(1);
   assert(InterlockedCompareExchange(&f->session.serial_waiters,0,0));
  }
  m.sequence=0;m.result=hr;m.payload_bytes=bytes;int sent=send_wake(&p,&m,out);if(sent!=PW_D3D9_OK){assert(pw_d3d9_channel_error(&p.channel));break;}
  if(stop){assert(pw_d3d9_channel_stopped(&p.channel)==PW_D3D9_OK);break;}
 }
 return 0;
}
static void begin(struct fixture *f,int fail)
{
 memset(f,0,sizeof(*f));f->fail=fail;struct pw_d3d9_session *s=&f->session;s->batch_next=1;{char value[2];s->async_enabled=GetEnvironmentVariableA("PW_D3D9_ASYNC",value,sizeof(value))==1&&value[0]=='1';}InitializeCriticalSection(&s->lock);
 s->serial_event=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.opened=CreateEventW(NULL,TRUE,FALSE,NULL);
 s->ipc.request=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.reply=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
 size_t bytes=pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES);if(mapped_wire){s->ipc.wire_mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)bytes,NULL);assert(s->ipc.wire_mapping);s->ipc.memory=MapViewOfFile(s->ipc.wire_mapping,FILE_MAP_ALL_ACCESS,0,0,bytes);assert(s->ipc.memory);}else s->ipc.memory=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes);
 assert(!pw_d3d9_channel_init(s->ipc.memory,bytes,1,RING_BYTES,RING_BYTES));assert(!pw_d3d9_channel_open(&s->ipc.channel,s->ipc.memory,bytes,1,PW_D3D9_CLIENT));
 f->thread=CreateThread(NULL,0,peer,f,0,NULL);assert(f->thread&&WaitForSingleObject(s->ipc.opened,1000)==WAIT_OBJECT_0);
 struct pw_d3d9_message q={.opcode=PW_D3D9_HELLO},r;assert(transact(s,&q,NULL,NULL,0,&r)==S_OK);
}
static HRESULT sync_call(struct fixture *f,unsigned opcode)
{struct pw_d3d9_message q={.opcode=opcode,.device=1,.object=7,.generation=1},r;return transact(&f->session,&q,NULL,NULL,0,&r);}
static HRESULT enqueue(struct fixture *f,unsigned target,unsigned value)
{struct pw_d3d9_command q={.method=57,.args={7,value}};return pw_d3d9_session_command(&f->session,(struct pw_d3d9_object_ref){target,1},&q);}
static void end(struct fixture *f)
{
 struct pw_d3d9_session *s=&f->session;assert(WaitForSingleObject(f->thread,1000)==WAIT_OBJECT_0);CloseHandle(f->thread);
 CloseHandle(s->ipc.opened);CloseHandle(s->ipc.request);CloseHandle(s->ipc.reply);CloseHandle(s->ipc.cancel);CloseHandle(s->serial_event);HeapFree(GetProcessHeap(),0,s->ipc.memory);DeleteCriticalSection(&s->lock);
}
static DWORD WINAPI producer(void *arg){struct fixture *f=arg;for(unsigned n=0;n<32;n++){assert(enqueue(f,7,n)==S_OK);if(!(n&7))assert(sync_call(f,PW_D3D9_GETTER_CALL)==S_OK);}return 0;}
static int scalar_control(void)
{
 SetEnvironmentVariableA("PW_D3D9_PROFILE","1");assert(InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL));struct fixture f;
 SetEnvironmentVariableA("PW_D3D9_ASYNC",NULL);begin(&f,0);assert(enqueue(&f,7,123)==S_OK&&f.commands==1&&f.batches==0&&f.session.ipc.stats.async_queued==0);assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);
 SetEnvironmentVariableA("PW_D3D9_ASYNC","1");
 begin(&f,0);struct pw_d3d9_command q={.method=57,.args={7,11}};assert(pw_d3d9_session_command(&f.session,(struct pw_d3d9_object_ref){7,1},&q)==S_OK);q.args[1]=99;
 assert(f.requests==1&&f.session.batch.count==1);callback_enter();assert(enqueue(&f,7,88)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();
 uint64_t previous=f.session.ipc.stats.roundtrip_wall_us,saved_frequency=profile_frequency;
 profile_frequency=1000000;clock_ticks=0;timed_clock=1;
 assert(sync_call(&f,PW_D3D9_GETTER_CALL)==S_OK&&f.commands==1&&f.values[0]==11);
 timed_clock=0;profile_frequency=saved_frequency;
 assert(f.session.ipc.stats.roundtrip_wall_us-previous==800); /* 300 batch + 500 outer, each interval once */
 assert(enqueue(&f,7,22)==S_OK&&enqueue(&f,8,33)==S_OK);assert(sync_call(&f,PW_D3D9_RELEASE)==S_OK&&f.values[1]==22&&f.values[2]==33);
 LONG before=f.batches;for(unsigned n=0;n<260;n++)assert(enqueue(&f,7,n)==S_OK);assert(sync_call(&f,PW_D3D9_GETTER_CALL)==S_OK&&f.batches-before==3);
 assert(enqueue(&f,7,44)==S_OK);ticks+=2;assert(enqueue(&f,7,55)==S_OK);assert(sync_call(&f,PW_D3D9_GETTER_CALL)==S_OK);
 HANDLE producers[4];for(unsigned n=0;n<4;n++){producers[n]=CreateThread(NULL,0,producer,&f,0,NULL);assert(producers[n]);}
 assert(WaitForMultipleObjects(4,producers,TRUE,3000)==WAIT_OBJECT_0);for(unsigned n=0;n<4;n++)CloseHandle(producers[n]);assert(sync_call(&f,PW_D3D9_GETTER_CALL)==S_OK);
 assert(enqueue(&f,7,66)==S_OK);struct pw_d3d9_message stop={.opcode=PW_D3D9_STOP},reply;assert(transact(&f.session,&stop,NULL,NULL,0,&reply)==S_OK);
 assert(f.session.ipc.stats.async_queued==394&&f.commands==394&&f.session.ipc.stats.batch_commands==394&&f.session.ipc.stats.batch_residence_wall_us>=2000);end(&f);
 begin(&f,1);for(unsigned n=0;n<3;n++)assert(enqueue(&f,7,n)==S_OK);assert(sync_call(&f,PW_D3D9_GETTER_CALL)==D3DERR_INVALIDCALL);assert(f.commands==2);
 assert(enqueue(&f,7,9)==D3DERR_INVALIDCALL&&sync_call(&f,PW_D3D9_RELEASE)==D3DERR_INVALIDCALL);end(&f);
 puts("BATCH_CLIENT PASS owned=1 sequence=1 capacity=1 age=1 multithread=1 release_stop_flush=1 sticky_failure=1");return 0;
}

struct ticket_context {struct fixture *f;LONG pins,drops;int close_on_drop;HRESULT close_hr;};
static struct ticket_context *active_context;
static void ticket_drop(void *opaque)
{
 struct ticket_context *c=opaque;struct pw_d3d9_session *s=&c->f->session;
 assert(s->lock.OwningThread!=(HANDLE)(ULONG_PTR)GetCurrentThreadId());
 assert(InterlockedDecrement(&c->pins)>=0);InterlockedIncrement(&c->drops);
 if(c->close_on_drop){
  c->close_on_drop=0;assert(pw_d3d9_session_close(s)==c->close_hr);assert(!free_count);
  assert(pw_d3d9_session_close(s)==E_FAIL&&!free_count); /* closing is coalesced while outer API owns storage */
  struct pw_d3d9_command q={.method=3};assert(pw_d3d9_session_command(s,(struct pw_d3d9_object_ref){7,1},&q)==E_FAIL);
 }
}
static HRESULT ticket_acquire(void *opaque,struct pw_d3d9_command *q,struct pw_d3d9_queue_ticket *ticket)
{
 struct ticket_context *c=opaque;assert(c->f->session.active_thread==GetCurrentThreadId());
 q->args[0]=123;q->args[1]=1;InterlockedIncrement(&c->pins);*ticket=(struct pw_d3d9_queue_ticket){c,ticket_drop};return S_OK;
}
static HRESULT binding(struct ticket_context *c)
{struct pw_d3d9_command q={.method=92};return pw_d3d9_session_binding(&c->f->session,(struct pw_d3d9_object_ref){7,1},&q,ticket_acquire,c);}
void pw_d3d9_session_test_callback(void)
{
 if(cancel_on_callback){cancel_on_callback=0;callback_enter();pw_d3d9_session_cancel(&active_context->f->session);
  assert(active_context->pins==1);assert(binding(active_context)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();}
}
static DWORD WINAPI malformed_contender(void *opaque)
{struct fixture *f=opaque;assert(WaitForSingleObject(f->enter,1000)==WAIT_OBJECT_0);SetEvent(f->arrived);assert(enqueue(f,7,1)==E_FAIL);return 0;}
static DWORD WINAPI release_contender(void *opaque)
{struct fixture *f=opaque;assert(WaitForSingleObject(f->enter,1000)==WAIT_OBJECT_0);SetEvent(f->arrived);assert(pw_d3d9_session_release(&f->session,(struct pw_d3d9_object_ref){123,1})==S_OK);return 0;}
static void binding_controls(void)
{
 struct fixture f;struct ticket_context c={.f=&f};active_context=&c;
 begin(&f,0);InterlockedExchange(&f.session.operation_holds,0x7fffffff);assert(binding(&c)==E_FAIL&&f.session.operation_holds==0x7fffffff);InterlockedExchange(&f.session.operation_holds,0);for(unsigned n=0;n<130;n++)assert(binding(&c)==S_OK);
 assert(c.pins==2&&c.drops==128);assert(pw_d3d9_session_release(&f.session,(struct pw_d3d9_object_ref){123,1})==S_OK);assert(!c.pins&&c.drops==130);
 assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);
 c=(struct ticket_context){.f=&f};begin(&f,0);assert(binding(&c)==S_OK);pw_d3d9_session_cancel(&f.session);assert(!c.pins&&c.drops==1);end(&f);
 c=(struct ticket_context){.f=&f};begin(&f,0);f.session.async_enabled=0;cancel_on_callback=1;assert(FAILED(binding(&c)));assert(!c.pins&&c.drops==1);end(&f);
 c=(struct ticket_context){.f=&f};begin(&f,2);f.session.async_enabled=0;f.enter=CreateEventW(NULL,TRUE,FALSE,NULL);f.arrived=CreateEventW(NULL,TRUE,FALSE,NULL);
 HANDLE contender=CreateThread(NULL,0,malformed_contender,&f,0,NULL);assert(contender);assert(binding(&c)==E_FAIL);assert(WaitForSingleObject(contender,1000)==WAIT_OBJECT_0);
 assert(!c.pins&&c.drops==1&&!f.session.batch.count);CloseHandle(contender);CloseHandle(f.enter);CloseHandle(f.arrived);end(&f);
 c=(struct ticket_context){.f=&f};begin(&f,3);f.session.async_enabled=0;f.enter=CreateEventW(NULL,TRUE,FALSE,NULL);f.arrived=CreateEventW(NULL,TRUE,FALSE,NULL);
 contender=CreateThread(NULL,0,release_contender,&f,0,NULL);assert(contender);assert(binding(&c)==S_OK);assert(WaitForSingleObject(contender,1000)==WAIT_OBJECT_0);
 assert(!c.pins&&c.drops==1);CloseHandle(contender);CloseHandle(f.enter);CloseHandle(f.arrived);assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);
 for(unsigned fail_receive=0;fail_receive<2;fail_receive++){
  begin(&f,0);struct pw_d3d9_command command={.method=3};unsigned char input[64],output[16];size_t bytes;
  assert(!pw_d3d9_command_encode(input,sizeof(input),&bytes,&command));memset(output,0xa5,sizeof(output));
  struct pw_d3d9_message q={.opcode=PW_D3D9_COMMAND_CALL,.device=1,.object=7,.generation=1,.payload_bytes=(uint32_t)bytes},reply;
  receive_failure=fail_receive;assert(transact(&f.session,&q,input,output,fail_receive?sizeof(output):0,&reply)==E_FAIL);
  assert(!reply.payload_bytes);for(unsigned n=0;n<sizeof(output);n++)assert(output[n]==0xa5);pw_d3d9_session_cancel(&f.session);end(&f);
 }
 for(unsigned cancel=0;cancel<2;cancel++){
  struct fixture *heap=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*heap));assert(heap);mapped_wire=1;begin(heap,0);
  heap->session.broker=heap->thread;watched_free=heap;free_count=0;c=(struct ticket_context){.f=heap,.close_on_drop=1,.close_hr=cancel?E_FAIL:S_OK};
  assert(binding(&c)==S_OK);
  if(cancel)pw_d3d9_session_cancel(&heap->session);
  else{struct pw_d3d9_command q={.method=3};assert(pw_d3d9_session_command(&heap->session,(struct pw_d3d9_object_ref){7,1},&q)==S_OK);}
  assert(free_count==1&&c.pins==0&&c.drops==1);watched_free=NULL;mapped_wire=0;
 }
 puts("BINDING_CLIENT PASS capacity_detach=1 idle_cancel=1 callback_cancel=1 malformed_contention=1 last_parent_close=1");
}
int main(void){
 assert((compiled_features()&(8192u|16384u|32768u))==(8192u|PW_D3D9_COMMAND_POLICY_FEATURE|32768u));
 assert(scalar_control()==0);binding_controls();return 0;
}
