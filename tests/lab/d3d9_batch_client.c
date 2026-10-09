/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <assert.h>
static ULONGLONG ticks=1;
static ULONGLONG WINAPI test_ticks(void){return ticks;}
#define GetTickCount64 test_ticks
#define PW_D3D9_ENABLE_METHODS
#define PW_D3D9_ENABLE_BATCH
#include "../../wine/ps5/d3d9/pw_d3d9_session.c"
#undef GetTickCount64
struct fixture {struct pw_d3d9_session session;HANDLE thread;LONG requests,batches,commands;UINT values[1024];int fail;};
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
    struct pw_d3d9_command q;assert(!pw_d3d9_batch_command(&q,&b,n)&&pw_d3d9_command_can_queue(&q));
    f->values[f->commands++]=q.args[1];r.attempted++;next++;
    if(f->fail&&n==1){r.failed_index=n;r.hresult=hr=D3DERR_INVALIDCALL;break;}
   }
   assert(!pw_d3d9_batch_reply_encode(out,sizeof(out),&r));bytes=sizeof(out);
  }
  if(m.opcode==PW_D3D9_COMMAND_CALL){
   struct pw_d3d9_command q;size_t written;
   assert(!pw_d3d9_command_decode(&q,scratch+64,m.payload_bytes));
   f->values[f->commands++]=q.args[1];
   assert(!pw_d3d9_command_reply_encode(out,sizeof(out),&written,q.method,S_OK));bytes=(unsigned)written;
  }
  m.sequence=0;m.result=hr;m.payload_bytes=bytes;assert(send_wake(&p,&m,out)==PW_D3D9_OK);
  if(stop){assert(pw_d3d9_channel_stopped(&p.channel)==PW_D3D9_OK);break;}
 }
 return 0;
}
static void begin(struct fixture *f,int fail)
{
 memset(f,0,sizeof(*f));f->fail=fail;struct pw_d3d9_session *s=&f->session;s->batch_next=1;{char value[2];s->async_enabled=GetEnvironmentVariableA("PW_D3D9_ASYNC",value,sizeof(value))==1&&value[0]=='1';}InitializeCriticalSection(&s->lock);
 s->serial_event=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.opened=CreateEventW(NULL,TRUE,FALSE,NULL);
 s->ipc.request=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.reply=CreateEventW(NULL,FALSE,FALSE,NULL);s->ipc.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
 size_t bytes=pw_d3d9_channel_bytes(RING_BYTES,RING_BYTES);s->ipc.memory=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes);
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
static DWORD WINAPI producer(void *arg){struct fixture *f=arg;for(unsigned n=0;n<32;n++)assert(enqueue(f,7,n)==S_OK);return 0;}
int main(void)
{
 SetEnvironmentVariableA("PW_D3D9_PROFILE","1");assert(InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL));struct fixture f;
 SetEnvironmentVariableA("PW_D3D9_ASYNC",NULL);begin(&f,0);assert(enqueue(&f,7,123)==S_OK&&f.commands==1&&f.batches==0&&f.session.ipc.stats.async_queued==0);assert(sync_call(&f,PW_D3D9_STOP)==S_OK);end(&f);
 SetEnvironmentVariableA("PW_D3D9_ASYNC","1");
 begin(&f,0);struct pw_d3d9_command q={.method=57,.args={7,11}};assert(pw_d3d9_session_command(&f.session,(struct pw_d3d9_object_ref){7,1},&q)==S_OK);q.args[1]=99;
 assert(f.requests==1&&f.session.batch.count==1);callback_enter();assert(enqueue(&f,7,88)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();
 assert(sync_call(&f,PW_D3D9_GETTER_CALL)==S_OK&&f.commands==1&&f.values[0]==11);
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
