/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <windows.h>
static unsigned clock_calls;
static BOOL WINAPI counted_clock(LARGE_INTEGER *out){clock_calls++;return QueryPerformanceCounter(out);}
#define QueryPerformanceCounter counted_clock
#include "../../wine/ps5/d3d9/pw_d3d9_session.c"
#undef QueryPerformanceCounter
#ifndef _WIN64
static DWORD WINAPI responder(void *arg)
{
 struct ipc *client=arg,peer={0};struct pw_d3d9_message m;unsigned char scratch[8192];
 peer.memory=client->memory;peer.request=client->request;peer.reply=client->reply;peer.cancel=client->cancel;
 assert(pw_d3d9_channel_open(&peer.channel,peer.memory,pw_d3d9_channel_bytes(8192,8192),77,PW_D3D9_SERVICE)==PW_D3D9_OK);
 SetEvent(client->opened);
 for(unsigned n=0;n<4;n++){
  assert(receive_wait(&peer,&m,scratch,sizeof(scratch),NULL)==PW_D3D9_OK);
  if(!n)assert(pw_d3d9_channel_ready(&peer.channel)==PW_D3D9_OK);
  m.sequence=0;m.payload_bytes=0;m.result=n==2?E_FAIL:S_OK;
  assert(send_wake(&peer,&m,NULL)==PW_D3D9_OK);
 }
 return 0;
}
#endif
int main(int argc,char **argv)
{
 int enabled=argc==2&&!strcmp(argv[1],"on");SetEnvironmentVariableA("PW_D3D9_PROFILE",enabled?"1":"0");
 assert(profile_enabled()==enabled);unsigned initial_clocks=clock_calls;
 struct ipc i={0};i.channel.epoch=9;
 struct pw_d3d9_transport_stats sample={.attempts=1,.published=1,.replies=1};sample.opcode[19]=1;
 struct pw_d3d9_message m={.object=2,.generation=3};struct profile_record r;
 profile_capture(&i,&sample,&m,5,E_FAIL,1,1,0,&r);
 if(enabled){assert(r.emit&&r.frame==1&&r.sequence==5&&r.status==(uint32_t)E_FAIL&&r.delta.attempts==1);profile_emit(&r);}
 else assert(!r.emit);
 m.object=4;profile_capture(&i,&sample,&m,6,S_OK,1,1,0,&r);
 if(enabled)assert(r.frame==2&&r.object==4&&r.delta.attempts==1&&r.total.attempts==2);
 profile_capture(&i,&sample,&m,0,E_FAIL,0,0,1,&r);
 if(enabled)assert(r.final&&r.frame==2&&r.total.attempts==3&&!r.reply_valid);
#ifndef _WIN64
 assert(InitOnceExecuteOnce(&tls_once,init_tls,NULL,NULL));
 struct pw_d3d9_session s={0};InitializeCriticalSection(&s.lock);
 s.serial_event=CreateEventW(NULL,FALSE,FALSE,NULL);s.ipc.opened=CreateEventW(NULL,TRUE,FALSE,NULL);
 s.ipc.request=CreateEventW(NULL,FALSE,FALSE,NULL);s.ipc.reply=CreateEventW(NULL,FALSE,FALSE,NULL);s.ipc.cancel=CreateEventW(NULL,TRUE,FALSE,NULL);
 size_t bytes=pw_d3d9_channel_bytes(8192,8192);s.ipc.memory=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes);
 assert(pw_d3d9_channel_init(s.ipc.memory,bytes,77,8192,8192)==PW_D3D9_OK);
 assert(pw_d3d9_channel_open(&s.ipc.channel,s.ipc.memory,bytes,77,PW_D3D9_CLIENT)==PW_D3D9_OK);
 HANDLE thread=CreateThread(NULL,0,responder,&s,0,NULL);assert(thread);assert(WaitForSingleObject(s.ipc.opened,1000)==WAIT_OBJECT_0);
 struct pw_d3d9_message reply,q={.opcode=PW_D3D9_HELLO};
 assert(transact(&s,&q,NULL,NULL,0,&reply)==S_OK);
 unsigned char present[8]={1,0,0,0,3,0,0,0};q=(struct pw_d3d9_message){.opcode=PW_D3D9_DEVICE_CALL,.device=1,.object=2,.generation=1,.payload_bytes=8};
 assert(transact(&s,&q,present,NULL,0,&reply)==S_OK);
 assert(transact(&s,&q,present,NULL,0,&reply)==E_FAIL);
 q.object=3;assert(transact(&s,&q,present,NULL,0,&reply)==S_OK);
 callback_enter();assert(transact(&s,&q,present,NULL,0,&reply)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL);callback_leave();
 if(enabled){assert(s.ipc.stats.attempts==5&&s.ipc.stats.published==4&&s.ipc.stats.replies==4&&s.ipc.stats.failures==2&&s.ipc.presents==3&&s.ipc.stats.rejected_present==1);assert(!s.ipc.stats.clock_invalid);
  struct pw_d3d9_transport_stats empty={0};profile_capture(&s.ipc,&empty,NULL,0,E_FAIL,0,0,1,&r);assert(r.delta.rejected_present==1&&r.final);profile_emit(&r);}
 assert(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0);CloseHandle(thread);
 CloseHandle(s.ipc.opened);CloseHandle(s.ipc.request);CloseHandle(s.ipc.reply);CloseHandle(s.ipc.cancel);CloseHandle(s.serial_event);HeapFree(GetProcessHeap(),0,s.ipc.memory);DeleteCriticalSection(&s.lock);
#endif
 if(!enabled)assert(clock_calls==initial_clocks);
 printf("TRANSPORT_PROFILE PASS enabled=%d clocks=%u\n",enabled,clock_calls);return 0;
}
