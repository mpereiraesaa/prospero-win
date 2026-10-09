/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
static LPTHREAD_START_ROUTINE peer_hook;
static HANDLE WINAPI create_test_thread(LPSECURITY_ATTRIBUTES a,SIZE_T bytes,LPTHREAD_START_ROUTINE start,LPVOID arg,DWORD flags,LPDWORD id)
{return CreateThread(a,bytes,peer_hook?peer_hook:start,arg,flags,id);}
#define CreateThread create_test_thread
#define PW_D3D9_ENABLE_BINDING_TICKETS
#define PW_D3D9_ENABLE_DRAW_BATCH
#define PW_D3D9_ENABLE_STATE_EVIDENCE
#define PW_D3D9_ENABLE_STATEBLOCK
#define PW_D3D9_ENABLE_OBJECT_GETTER
#define PW_D3D9_ENABLE_DEVICE
static int observer_bad_reply;
#define pw_d3d9_command_reply_encode observer_encode_reply
#define main draw_client_baseline_main
#include "d3d9_batch_client.c"
#undef main
#undef CreateThread
#undef pw_d3d9_command_reply_encode
extern int pw_d3d9_command_reply_encode(void *,size_t,size_t *,uint32_t,uint32_t);
int observer_encode_reply(void *out,size_t capacity,size_t *bytes,uint32_t method,uint32_t hr)
{return pw_d3d9_command_reply_encode(out,capacity,bytes,observer_bad_reply?57u:method,hr);}
struct observation {struct fixture *fixture;unsigned queued,completed,eligible,dropped,answers;int fail,fail_queued,known;unsigned char matrix[64];};
static void admitted(struct observation *o)
{assert(o->fixture->session.active_thread==GetCurrentThreadId());assert(pw_d3d9_channel_state(&o->fixture->session.ipc.channel)==PW_D3D9_READY);}
static int eligible(void *opaque,const struct pw_d3d9_command *q)
{struct observation *o=opaque;admitted(o);(void)q;o->eligible++;return 0;}
static HRESULT queued(void *opaque,const struct pw_d3d9_command *q)
{struct observation *o=opaque;admitted(o);assert(o->fixture->session.batch.count&&(q->method==57||q->method==87||q->method==44));o->queued++;
 if(q->method==44){memcpy(o->matrix,q->data.bytes,64);o->known=1;}
 return o->fail_queued?E_FAIL:S_OK;}
static HRESULT completed(void *opaque,uint32_t opcode,const void *input,size_t input_bytes,const void *output,size_t output_bytes,HRESULT hr)
{
 struct observation *o=opaque;admitted(o);struct pw_d3d9_command q;uint32_t method,result;
 if(opcode==PW_D3D9_GETTER_CALL){
  struct pw_d3d9_getter_request request;struct pw_d3d9_getter_reply reply;
  if(pw_d3d9_getter_decode(&request,input,input_bytes)||pw_d3d9_getter_reply_decode(&reply,&request,output,output_bytes)||reply.hresult!=(uint32_t)hr)return E_FAIL;
  assert(request.method==45&&reply.bytes==64);memcpy(o->matrix,reply.data.bytes,64);o->known=1;o->completed++;return S_OK;
 }
 assert(opcode==PW_D3D9_COMMAND_CALL&&!pw_d3d9_command_decode(&q,input,input_bytes));
 assert(!pw_d3d9_command_reply_decode(&method,&result,output,output_bytes)&&method==q.method&&result==(uint32_t)hr);
 o->completed++;return o->fail?E_FAIL:S_OK;
}
static unsigned char cache_native[64];
static DWORD WINAPI cache_peer(void *arg)
{
 struct fixture *f=arg;struct ipc p={0};struct pw_d3d9_message m;unsigned char scratch[RING_BYTES],out[PW_D3D9_GETTER_MAX];uint64_t next=1;
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
    struct pw_d3d9_command q;assert(!pw_d3d9_batch_command(&q,&b,n)&&pw_d3d9_command_can_queue(&q));
    f->values[f->commands++]=q.args[1];if(q.method==44)memcpy(cache_native,q.data.bytes,64);r.attempted++;next++;
    if(f->fail&&n==1){r.failed_index=n;r.hresult=hr=D3DERR_INVALIDCALL;break;}
   }
   assert(!pw_d3d9_batch_reply_encode(out,sizeof(out),&r));bytes=PW_D3D9_BATCH_REPLY;
  }
  if(m.opcode==PW_D3D9_COMMAND_CALL){
   struct pw_d3d9_command q;size_t written;
   assert(!pw_d3d9_command_decode(&q,scratch+64,m.payload_bytes));
   f->values[f->commands++]=q.args[1];if(q.method==44)memcpy(cache_native,q.data.bytes,64);
   assert(!pw_d3d9_command_reply_encode(out,sizeof(out),&written,q.method,S_OK));bytes=(unsigned)written;
  }
  if(m.opcode==PW_D3D9_GETTER_CALL){
   struct pw_d3d9_getter_request q;struct pw_d3d9_getter_reply r={.method=45,.hresult=S_OK,.bytes=64};size_t written;
   assert(!pw_d3d9_getter_decode(&q,scratch+64,m.payload_bytes)&&q.method==45);
   memcpy(r.data.bytes,cache_native,64);assert(!pw_d3d9_getter_reply_encode(out,sizeof(out),&written,&q,&r));bytes=(unsigned)written;
  }
  m.sequence=0;m.result=hr;m.payload_bytes=bytes;assert(send_wake(&p,&m,out)==PW_D3D9_OK);
  if(stop){assert(pw_d3d9_channel_stopped(&p.channel)==PW_D3D9_OK);break;}
 }
 return 0;
}

static int answer(void *opaque,const struct pw_d3d9_getter_request *q,struct pw_d3d9_getter_reply *r)
{
 struct observation *o=opaque;admitted(o);o->answers++;assert(q->method==45);
 if(!o->known)return 0;
 *r=(struct pw_d3d9_getter_reply){.method=45,.hresult=S_OK,.bytes=64};memcpy(r->data.bytes,o->matrix,64);return 1;
}
static void drop_ticket(void *opaque)
{struct observation *o=opaque;assert(!o->fixture->session.active_thread);o->dropped++;}
static HRESULT acquire_ticket(void *opaque,struct pw_d3d9_command *q,struct pw_d3d9_queue_ticket *ticket)
{struct observation *o=opaque;admitted(o);q->args[0]=9;q->args[1]=1;*ticket=(struct pw_d3d9_queue_ticket){.context=o,.drop=drop_ticket};return S_OK;}
static void cache_controls(void)
{
 struct fixture f;struct observation o={.fixture=&f};struct pw_d3d9_session_observer observer={&o,completed,eligible,queued,answer};
 struct pw_d3d9_object_ref ref={7,1};struct pw_d3d9_getter_request get={.method=45,.args={256}};struct pw_d3d9_getter_reply result;
 struct pw_d3d9_command set={.method=44,.args={256},.data_bytes=64};for(unsigned i=0;i<64;i++)set.data.bytes[i]=(unsigned char)(i*3+1);
 peer_hook=cache_peer;memset(cache_native,0,sizeof(cache_native));begin(&f,0);
 assert(pw_d3d9_session_command_observed(&f.session,ref,&set,&observer)==S_OK&&f.session.batch.count==1);
 LONG requests=f.requests;for(unsigned n=0;n<3;n++){
  assert(pw_d3d9_session_getter_observed(&f.session,ref,&get,&result,&observer)==S_OK&&!memcmp(result.data.bytes,set.data.bytes,64));
  assert(f.requests==requests&&!f.batches&&f.session.batch.count==1);
 }
 callback_enter();assert(pw_d3d9_session_getter_observed(&f.session,ref,&get,&result,&observer)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();assert(o.answers==3);
 o.known=0;assert(pw_d3d9_session_getter_observed(&f.session,ref,&get,&result,&observer)==S_OK&&!memcmp(result.data.bytes,set.data.bytes,64));
 assert(f.batches==1&&f.requests==requests+2&&o.completed==1);
 assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);
 o=(struct observation){.fixture=&f,.known=1};SetEnvironmentVariableA("PW_D3D9_ASYNC",NULL);begin(&f,0);
 assert(pw_d3d9_session_getter_observed(&f.session,ref,&get,&result,&observer)==S_OK&&!o.answers&&o.completed==1&&f.requests==2);
 assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);SetEnvironmentVariableA("PW_D3D9_ASYNC","1");
 o=(struct observation){.fixture=&f};begin(&f,1);
 for(unsigned n=0;n<3;n++)assert(pw_d3d9_session_command_observed(&f.session,ref,&set,&observer)==S_OK);
 assert(sync_call(&f,PW_D3D9_GETTER_CALL)==D3DERR_INVALIDCALL);
 assert(pw_d3d9_session_getter_observed(&f.session,ref,&get,&result,&observer)==D3DERR_INVALIDCALL&&!o.answers);end(&f);
 peer_hook=NULL;o=(struct observation){.fixture=&f,.fail_queued=1};begin(&f,0);
 struct pw_d3d9_command binding={.method=87};
 assert(pw_d3d9_session_binding_observed(&f.session,ref,&binding,acquire_ticket,&o,&observer)==E_FAIL&&o.dropped==1&&o.queued==1&&!f.batches);end(&f);
}
int main(void)
{
 assert(!draw_client_baseline_main());
 struct fixture f;struct observation o={.fixture=&f};
 struct pw_d3d9_session_observer observer={&o,completed,eligible,queued,NULL};
 struct pw_d3d9_command q={.method=57,.args={7,1}},sync={.method=89,.args={0}};
 struct pw_d3d9_object_ref ref={7,1};
 begin(&f,0);assert(pw_d3d9_session_command_observed(&f.session,ref,&q,&observer)==S_OK&&o.queued==1&&!o.completed);
 callback_enter();assert(pw_d3d9_session_command_observed(&f.session,ref,&q,&observer)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();
 assert(o.queued==1&&!o.completed);
 assert(pw_d3d9_session_command_observed(&f.session,ref,&sync,&observer)==S_OK&&o.completed==1&&f.commands==2);
 assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);
 o=(struct observation){.fixture=&f};begin(&f,1);
 for(unsigned n=0;n<3;n++)assert(pw_d3d9_session_command_observed(&f.session,ref,&q,&observer)==S_OK);
 assert(pw_d3d9_session_command_observed(&f.session,ref,&sync,&observer)==D3DERR_INVALIDCALL&&!o.completed);
 assert(pw_d3d9_session_command_observed(&f.session,ref,&q,&observer)==D3DERR_INVALIDCALL&&o.queued==3&&!o.completed);end(&f);
 o=(struct observation){.fixture=&f};begin(&f,0);observer_bad_reply=1;
 assert(pw_d3d9_session_command_observed(&f.session,ref,&sync,&observer)==E_FAIL&&!o.completed);end(&f);observer_bad_reply=0;
 o=(struct observation){.fixture=&f,.fail=1};begin(&f,0);
 assert(pw_d3d9_session_command_observed(&f.session,ref,&sync,&observer)==E_FAIL&&o.completed==1);end(&f);
 cache_controls();
 puts("DRAW_CLIENT PASS admitted=1 queued_after_append=1 callback_reject=1 ordered_flush=1 sticky=1 typed_before_observer=1 observer_failure=1");return 0;
}
