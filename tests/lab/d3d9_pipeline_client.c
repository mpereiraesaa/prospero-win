/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <assert.h>
#include <pw_d3d9_bridge_wire.h>
static int corrupt_ack, fail_allocation, fail_wake, cancel_inside;
static HANDLE watched_request;
static int injected_receive(struct pw_d3d9_channel *,struct pw_d3d9_message *,void *,size_t);
static LPVOID WINAPI injected_alloc(HANDLE,DWORD,SIZE_T);
static BOOL WINAPI injected_event(HANDLE);
static int injected_sleeping(const struct pw_d3d9_channel *);
#define pw_d3d9_channel_receive injected_receive
#define HeapAlloc injected_alloc
#define SetEvent injected_event
#define pw_d3d9_channel_peer_sleeping injected_sleeping
#define PW_D3D9_ENABLE_METHODS
#define PW_D3D9_ENABLE_BATCH
#define PW_D3D9_ENABLE_BINDING_TICKETS
#define PW_D3D9_ENABLE_PIPELINE
#define PW_D3D9_ENABLE_DEVICE
#define PW_D3D9_ENABLE_DRAW_BATCH
#define PW_D3D9_ENABLE_STATEBLOCK
#define PW_D3D9_ENABLE_OBJECT_GETTER
#define PW_D3D9_ENABLE_STATE_EVIDENCE
#ifndef PIPELINE_SESSION_SOURCE
#define PIPELINE_SESSION_SOURCE "../../wine/ps5/d3d9/pw_d3d9_session.c"
#endif
static ULONGLONG WINAPI fixed_ticks(void){return 1;}
#define GetTickCount64 fixed_ticks
#include PIPELINE_SESSION_SOURCE
#undef GetTickCount64
#undef pw_d3d9_channel_receive
#undef HeapAlloc
#undef SetEvent
#undef pw_d3d9_channel_peer_sleeping
static int injected_receive(struct pw_d3d9_channel *c,struct pw_d3d9_message *m,void *data,size_t size)
{
 int result=pw_d3d9_channel_receive(c,m,data,size);
 if(corrupt_ack&&c->role==PW_D3D9_CLIENT&&result==PW_D3D9_OK&&m->opcode==PW_D3D9_COMMAND_BATCH_CALL){
  if(corrupt_ack==1)((unsigned char *)data)[64]^=1;else m->ticket++;
  corrupt_ack=0;
 }
 return result;
}
static LPVOID WINAPI injected_alloc(HANDLE heap,DWORD flags,SIZE_T bytes)
{if(fail_allocation&&bytes==sizeof(struct pipeline_tickets)){fail_allocation=0;return NULL;}return HeapAlloc(heap,flags,bytes);}
static BOOL WINAPI injected_event(HANDLE event)
{if(fail_wake&&event==watched_request){fail_wake=0;SetLastError(ERROR_INVALID_HANDLE);return FALSE;}return SetEvent(event);}
static int injected_sleeping(const struct pw_d3d9_channel *c)
{return fail_wake&&c->role==PW_D3D9_CLIENT?1:pw_d3d9_channel_peer_sleeping(c);}

struct fixture {
 struct pw_d3d9_session session;
 HANDLE peer, first, execute, cancelled, exit_peer, producer_done, ack;
 LONG returned, requests, batches, executed, pins, drops, peer_done;
 unsigned expected_before_barrier;
 int fail, hold_exit;
 LONG answers;
 int answer_fault;
 HRESULT producer_result;
};
static LONG read_long(LONG *p){return InterlockedCompareExchange(p,0,0);}
static DWORD WINAPI wire_peer(void *opaque)
{
 struct fixture *f=opaque;struct ipc p={0};struct pw_d3d9_message m;
 unsigned char scratch[8192],output[PW_D3D9_GETTER_MAX];uint64_t next=1;
 p.memory=f->session.ipc.memory;p.request=f->session.ipc.request;p.reply=f->session.ipc.reply;p.cancel=f->session.ipc.cancel;
 size_t size=pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES);
 assert(!pw_d3d9_channel_open(&p.channel,p.memory,size,1,PW_D3D9_SERVICE));SetEvent(f->session.ipc.opened);
 while(receive_wait(&p,&m,scratch,sizeof(scratch),NULL)==PW_D3D9_OK){
  InterlockedIncrement(&f->requests);size_t bytes=0;HRESULT hr=S_OK;int stop=m.opcode==PW_D3D9_STOP;
  if(m.opcode==PW_D3D9_HELLO)assert(!pw_d3d9_channel_ready(&p.channel));
  else if(m.opcode==PW_D3D9_COMMAND_BATCH_CALL){
   struct pw_d3d9_command_batch batch;assert(!pw_d3d9_batch_decode(&batch,scratch+64,m.payload_bytes));
   assert(batch.first_sequence==next);LONG number=InterlockedIncrement(&f->batches);
   if(number==1){SetEvent(f->first);assert(WaitForSingleObject(f->execute,10000)==WAIT_OBJECT_0);}
   struct pw_d3d9_batch_reply reply={next,batch.count,0,UINT32_MAX,0};
   for(unsigned i=0;i<batch.count;i++){
    struct pw_d3d9_command q;assert(!pw_d3d9_batch_command(&q,&batch,i));
    assert(q.method==92&&q.args[0]==123&&q.args[1]==1);InterlockedIncrement(&f->executed);reply.attempted++;next++;
    if(f->fail==1&&i==1){reply.failed_index=i;reply.hresult=(uint32_t)(hr=D3DERR_INVALIDCALL);break;}
   }
   assert(!pw_d3d9_batch_reply_encode(output,sizeof(output),&reply));bytes=PW_D3D9_BATCH_REPLY;
  }else{
   assert((unsigned)read_long(&f->executed)==f->expected_before_barrier);
   if(m.opcode==PW_D3D9_GETTER_CALL){
    struct pw_d3d9_getter_request q;struct pw_d3d9_getter_reply r={.method=45,.hresult=S_OK,.bytes=64};
    assert(!pw_d3d9_getter_decode(&q,scratch+64,m.payload_bytes)&&q.method==45);
    assert(!pw_d3d9_getter_reply_encode(output,sizeof(output),&bytes,&q,&r));
   }
   if(m.opcode==PW_D3D9_DEVICE_CALL){
    struct pw_d3d9_device_request q;struct pw_d3d9_device_reply r={0};
    assert(!pw_d3d9_device_request_decode(&q,scratch+64,m.payload_bytes));r.operation=q.operation;r.hresult=S_OK;r.parameters=q.parameters;
    assert(!pw_d3d9_device_reply_encode(output,sizeof(output),&bytes,&r));
   }
  }
  m.sequence=0;m.result=hr;m.payload_bytes=(uint32_t)bytes;
  if(send_wake(&p,&m,output)!=PW_D3D9_OK)break;
  if(f->fail&&m.opcode==PW_D3D9_COMMAND_BATCH_CALL){
   SetEvent(f->ack);
   assert(WaitForSingleObject(p.cancel,10000)==WAIT_OBJECT_0);SetEvent(f->cancelled);
   if(f->hold_exit)assert(WaitForSingleObject(f->exit_peer,10000)==WAIT_OBJECT_0);
   break;
  }
  if(stop){assert(!pw_d3d9_channel_stopped(&p.channel));break;}
 }
 InterlockedExchange(&f->peer_done,1);return 0;
}
static void ticket_drop(void *opaque)
{
 struct fixture *f=opaque;struct pw_d3d9_session *s=&f->session;
 assert(s->lock.OwningThread!=(HANDLE)(ULONG_PTR)GetCurrentThreadId());
 assert(!s->active_thread);assert(InterlockedDecrement(&f->pins)>=0);InterlockedIncrement(&f->drops);
}
static HRESULT ticket_acquire(void *opaque,struct pw_d3d9_command *q,struct pw_d3d9_queue_ticket *ticket)
{
 struct fixture *f=opaque;assert(f->session.active_thread==GetCurrentThreadId());
 q->args[0]=123;q->args[1]=1;InterlockedIncrement(&f->pins);
 *ticket=(struct pw_d3d9_queue_ticket){f,ticket_drop};
 if(cancel_inside){cancel_inside=0;pw_d3d9_session_cancel(&f->session);}
 return S_OK;
}
static HRESULT binding(struct fixture *f)
{
 struct pw_d3d9_command q={.method=92};
 return pw_d3d9_session_binding(&f->session,(struct pw_d3d9_object_ref){7,1},&q,ticket_acquire,f);
}
static HRESULT barrier(struct fixture *f,unsigned opcode)
{
 struct pw_d3d9_message q={.opcode=opcode,.device=1,.object=7,.generation=1},reply;
 return transact(&f->session,&q,NULL,NULL,0,&reply);
}
static void begin(struct fixture *f,int fail)
{
 memset(f,0,sizeof(*f));f->fail=fail;struct pw_d3d9_session *s=&f->session;
 s->async_enabled=1;s->batch_next=1;InitializeCriticalSection(&s->lock);
 s->serial_event=CreateEventW(NULL,FALSE,FALSE,NULL);
 s->ipc.opened=CreateEventW(NULL,TRUE,FALSE,NULL);s->ipc.request=CreateEventW(NULL,FALSE,FALSE,NULL);
 s->ipc.reply=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
 f->ack=CreateEventW(NULL,TRUE,FALSE,NULL);f->first=CreateEventW(NULL,TRUE,FALSE,NULL);f->execute=CreateEventW(NULL,TRUE,FALSE,NULL);
 f->cancelled=CreateEventW(NULL,TRUE,FALSE,NULL);f->exit_peer=CreateEventW(NULL,TRUE,FALSE,NULL);
 f->producer_done=CreateEventW(NULL,TRUE,FALSE,NULL);
 size_t size=pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES);s->ipc.memory=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);assert(s->ipc.memory);
 assert(!pw_d3d9_channel_init(s->ipc.memory,size,1,RING_BYTES,RING_BYTES));
 assert(!pw_d3d9_channel_open(&s->ipc.channel,s->ipc.memory,size,1,PW_D3D9_CLIENT));
 f->peer=CreateThread(NULL,0,wire_peer,f,0,NULL);s->broker=f->peer;
 assert(f->peer&&WaitForSingleObject(s->ipc.opened,10000)==WAIT_OBJECT_0);
 struct pw_d3d9_message hello={.opcode=PW_D3D9_HELLO},reply;assert(transact(s,&hello,NULL,NULL,0,&reply)==S_OK);
}
static void finish(struct fixture *f)
{
 assert(WaitForSingleObject(f->peer,10000)==WAIT_OBJECT_0&&read_long(&f->peer_done));
 assert(!read_long(&f->pins)&&!f->session.pipeline.count&&!f->session.pipeline_blocks);
 HANDLE handles[]={f->ack,f->peer,f->first,f->execute,f->cancelled,f->exit_peer,f->producer_done,
 f->session.ipc.opened,f->session.ipc.request,f->session.ipc.reply,f->session.ipc.cancel,f->session.serial_event};
 for(unsigned i=0;i<sizeof(handles)/sizeof(handles[0]);i++)CloseHandle(handles[i]);
 HeapFree(GetProcessHeap(),0,f->session.ipc.memory);DeleteCriticalSection(&f->session.lock);
}
static DWORD WINAPI fill(void *opaque)
{
 struct fixture *f=opaque;f->producer_result=S_OK;
 for(unsigned i=0;i<1153;i++){
  HRESULT hr=binding(f);if(FAILED(hr)){f->producer_result=hr;break;}
  InterlockedIncrement(&f->returned);
 }
 SetEvent(f->producer_done);return 0;
}
static void capacity_and_barriers(void)
{
 assert(RING_BYTES==65536&&FRAME_BYTES==8192);
 struct fixture f;begin(&f,0);HANDLE producer=CreateThread(NULL,0,fill,&f,0,NULL);assert(producer);
 assert(WaitForSingleObject(f.first,10000)==WAIT_OBJECT_0);
 DWORD start=GetTickCount();while(read_long(&f.returned)<1152&&GetTickCount()-start<10000)Sleep(1);
 assert(read_long(&f.returned)==1152&&WaitForSingleObject(f.producer_done,0)==WAIT_TIMEOUT);
 assert(!read_long(&f.executed)&&!read_long(&f.drops));
 /* The ninth publication cannot return before one of the eight credits retires. */
 SetEvent(f.execute);assert(WaitForSingleObject(producer,10000)==WAIT_OBJECT_0);
 assert(f.producer_result==S_OK&&read_long(&f.returned)==1153);CloseHandle(producer);
 f.expected_before_barrier=1153;
 struct pw_d3d9_getter_request get={.method=45,.args={256}};struct pw_d3d9_getter_reply got;
 assert(pw_d3d9_session_getter(&f.session,(struct pw_d3d9_object_ref){7,1},&get,&got)==S_OK);
 assert(!read_long(&f.pins)&&read_long(&f.drops)==1153);
 for(unsigned op=PW_D3D9_DEVICE_RESET;op<=PW_D3D9_DEVICE_PRESENT;op++){
  assert(binding(&f)==S_OK);f.expected_before_barrier++;
  struct pw_d3d9_device_request q={.operation=op};struct pw_d3d9_device_reply reply;
  assert(pw_d3d9_session_device(&f.session,(struct pw_d3d9_object_ref){7,1},&q,&reply)==S_OK);
  assert(!read_long(&f.pins));
 }
 const unsigned barriers[]={PW_D3D9_RELEASE,PW_D3D9_STOP};
 for(unsigned i=0;i<sizeof(barriers)/sizeof(barriers[0]);i++){
  assert(binding(&f)==S_OK);f.expected_before_barrier++;assert(barrier(&f,barriers[i])==S_OK);assert(!read_long(&f.pins));
 }
 finish(&f);
}
static HRESULT cache_complete(void *opaque,uint32_t op,const void *in,size_t ins,const void *out,size_t outs,HRESULT hr)
{(void)opaque;(void)op;(void)in;(void)ins;(void)out;(void)outs;return hr;}
static int cache_answer(void *opaque,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{
 struct fixture *f=opaque;assert(f->session.active_thread==GetCurrentThreadId());InterlockedIncrement(&f->answers);
 *r=(struct pw_d3d9_getter_reply){.method=q->method,.hresult=S_OK,.bytes=f->answer_fault==3?60:64};
 return f->answer_fault==1?-1:f->answer_fault==2?2:1;
}
static HRESULT cached(struct fixture *f)
{
 struct pw_d3d9_session_observer observer={.context=f,.completed=cache_complete,.answer=cache_answer};
 struct pw_d3d9_getter_request q={.method=45,.args={256}};struct pw_d3d9_getter_reply r;
 return pw_d3d9_session_getter_observed(&f->session,(struct pw_d3d9_object_ref){7,1},&q,&r,&observer);
}
static DWORD WINAPI failure_consumer(void *opaque)
{
 struct fixture *f=opaque;f->producer_result=cached(f);SetEvent(f->producer_done);return 0;
}
static void failure_and_quarantine(unsigned fault)
{
 struct fixture f;begin(&f,fault?2:1);f.hold_exit=1;corrupt_ack=(int)fault;
 /* 257 calls publish two full batches and retain one unpublished command. */
 for(unsigned i=0;i<257;i++)assert(binding(&f)==S_OK);
 assert(WaitForSingleObject(f.first,10000)==WAIT_OBJECT_0&&!read_long(&f.executed));
 callback_enter();assert(binding(&f)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);
 assert(cached(&f)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();
 EnterCriticalSection(&f.session.lock);f.session.active_thread=GetCurrentThreadId();
 assert(binding(&f)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL&&cached(&f)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);
 f.session.active_thread=0;LeaveCriticalSection(&f.session.lock);
 assert(!read_long(&f.answers));SetEvent(f.execute);
 /* Wait for the failure ACK to be published without consuming it. */
 assert(WaitForSingleObject(f.ack,10000)==WAIT_OBJECT_0&&read_long(&f.executed)==(fault?128:2));
 /* cached() must harvest the pending failure before invoking its answer hook. */
 HANDLE consumer=CreateThread(NULL,0,failure_consumer,&f,0,NULL);assert(consumer);
 assert(WaitForSingleObject(f.cancelled,10000)==WAIT_OBJECT_0);
 assert(!read_long(&f.answers)&&read_long(&f.batches)==1);
 assert(WaitForSingleObject(f.producer_done,0)==WAIT_TIMEOUT&&!read_long(&f.peer_done));
 assert(read_long(&f.pins)>=128); /* Published unexecuted suffix remains pinned until join. */
 SetEvent(f.exit_peer);assert(WaitForSingleObject(consumer,10000)==WAIT_OBJECT_0);CloseHandle(consumer);
 HRESULT expected=fault?E_FAIL:D3DERR_INVALIDCALL;
 assert(f.producer_result==expected&&read_long(&f.executed)==(fault?128:2)&&read_long(&f.drops)==257);
 assert(FAILED(binding(&f))&&f.session.batch_failure==expected);
 assert(read_long(&f.executed)==(fault?128:2)&&read_long(&f.drops)==257&&!read_long(&f.pins));finish(&f);
}
static void publication_faults(void)
{
 for(unsigned wake=0;wake<2;wake++){
  struct fixture f;begin(&f,0);SetEvent(f.execute);
  for(unsigned n=0;n<128;n++)assert(binding(&f)==S_OK);
  watched_request=f.session.ipc.request;fail_wake=(int)wake;fail_allocation=!wake;
  assert(FAILED(binding(&f)));assert(!fail_wake&&!fail_allocation);
  assert(read_long(&f.drops)==129&&!read_long(&f.pins));
  assert(WaitForSingleObject(f.peer,10000)==WAIT_OBJECT_0);finish(&f);watched_request=NULL;
 }
}
static void cached_failure_outputs(void)
{
 for(unsigned fault=0;fault<4;fault++){
  struct fixture f;begin(&f,0);f.answer_fault=(int)fault;
  struct pw_d3d9_session_observer observer={.context=&f,.completed=cache_complete,.answer=cache_answer};
  struct pw_d3d9_getter_request q={.method=45,.args={256}};struct pw_d3d9_getter_reply output,before;
  memset(&output,0xa5,sizeof(output));before=output;
  if(!fault){cancel_ipc(&f.session.ipc);assert(!f.session.batch_failure);}
  HRESULT hr=pw_d3d9_session_getter_observed(&f.session,(struct pw_d3d9_object_ref){7,1},&q,&output,&observer);
  fprintf(stderr,"PIPELINE_CACHE_NEGATIVE case=%u hr=%08lx unchanged=%d\n",fault,(unsigned long)hr,!memcmp(&output,&before,sizeof(output)));
  assert(FAILED(hr)&&!memcmp(&output,&before,sizeof(output)));
  assert(read_long(&f.answers)==(fault?1:0));finish(&f);
 }
}
static DWORD WINAPI cancel_operation(void *opaque)
{
 struct fixture *f=opaque;
 if(cancel_inside)f->producer_result=binding(f);else{pw_d3d9_session_cancel(&f->session);f->producer_result=E_FAIL;}
 SetEvent(f->producer_done);return 0;
}
static void cancellation_modes(void)
{
 for(unsigned mode=0;mode<3;mode++){
  struct fixture f;begin(&f,0);for(unsigned i=0;i<129;i++)assert(binding(&f)==S_OK);
  assert(WaitForSingleObject(f.first,10000)==WAIT_OBJECT_0&&read_long(&f.pins)==129);
  if(mode==1){callback_enter();pw_d3d9_session_cancel(&f.session);assert(read_long(&f.pins)==129);callback_leave();}
  cancel_inside=mode==2;
  HANDLE thread=CreateThread(NULL,0,cancel_operation,&f,0,NULL);assert(thread);
  assert(WaitForSingleObject(f.session.ipc.cancel,10000)==WAIT_OBJECT_0);
  assert(WaitForSingleObject(f.producer_done,0)==WAIT_TIMEOUT&&!read_long(&f.peer_done));
  assert(read_long(&f.pins)==(mode==2?130:129));
  SetEvent(f.execute);assert(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);CloseHandle(thread);
  assert(FAILED(f.producer_result)&&read_long(&f.drops)==(mode==2?130:129));finish(&f);
 }
}
static void maximum_frames(void)
{
 unsigned char payload[PW_D3D9_BATCH_MAX],scratch[FRAME_BYTES];size_t bytes;
 struct pw_d3d9_command_batch batch;struct pw_d3d9_command q={.method=94,.args={0,256},.data_bytes=4096};
 assert(!pw_d3d9_batch_init(&batch,1)&&!pw_d3d9_batch_append(&batch,&q));
 q.args[1]=242;q.data_bytes=3872;assert(!pw_d3d9_batch_append(&batch,&q));
 assert(!pw_d3d9_batch_encode(payload,sizeof(payload),&bytes,&batch)&&bytes==8064);
 size_t capacity=pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES);void *memory=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,capacity);assert(memory);
 struct pw_d3d9_channel client,service;assert(!pw_d3d9_channel_init(memory,capacity,7,RING_BYTES,RING_BYTES));
 assert(!pw_d3d9_channel_open(&client,memory,capacity,7,PW_D3D9_CLIENT));assert(!pw_d3d9_channel_open(&service,memory,capacity,7,PW_D3D9_SERVICE));
 struct pw_d3d9_message hello={.opcode=PW_D3D9_HELLO},reply;
 assert(!pw_d3d9_channel_send(&client,&hello,NULL)&&!pw_d3d9_channel_receive(&service,&reply,scratch,sizeof(scratch)));
 assert(!pw_d3d9_channel_ready(&service));reply.sequence=0;
 assert(!pw_d3d9_channel_send(&service,&reply,NULL)&&!pw_d3d9_channel_receive(&client,&hello,scratch,sizeof(scratch)));
 struct pw_d3d9_message message={.opcode=PW_D3D9_COMMAND_BATCH_CALL,.device=1,.object=7,.generation=1,.payload_bytes=(uint32_t)bytes};
 for(unsigned n=0;n<8;n++)assert(!pw_d3d9_channel_send(&client,&message,payload));
 uint64_t sequence=client.next_send;uint32_t pending=client.pending_count;
 assert(pending==8&&pw_d3d9_channel_send(&client,&message,payload)==PW_D3D9_FULL);
 assert(client.next_send==sequence&&client.pending_count==pending&&!pw_d3d9_channel_error(&client));
 for(unsigned n=0;n<8;n++){
  assert(!pw_d3d9_channel_receive(&service,&reply,scratch,sizeof(scratch))&&reply.payload_bytes==8064);
  assert(!memcmp(scratch+64,payload,bytes));
 }
 assert(pw_d3d9_channel_receive(&service,&reply,scratch,sizeof(scratch))==PW_D3D9_EMPTY);
 pw_d3d9_channel_cancel(&client,1);HeapFree(GetProcessHeap(),0,memory);
}
int main(void)
{
 assert(InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL));SetEnvironmentVariableA("PW_D3D9_ASYNC","1");
 maximum_frames();cancellation_modes();cached_failure_outputs();capacity_and_barriers();for(unsigned fault=0;fault<3;fault++)failure_and_quarantine(fault);publication_faults();
 puts("PIPELINE_CLIENT PASS capacity=8 ninth_backpressure=1 contiguous=1 barriers=1 ticket_callbacks=1 failure_prefix=2 quarantine_join=1 cached_health=1 callback_denial=1 malformed_ack=1 out_of_order_ack=1 allocation_failure=1 published_wake_failure=1 cache_failure_output=1 max_frames=8 full_preserves_sequence=1 cancel_idle_callback_inflight=1");return 0;
}
