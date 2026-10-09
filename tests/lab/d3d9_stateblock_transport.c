/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {HRESULT actual=(x);if(FAILED(actual)){fprintf(stderr,"FAIL line=%d hr=%08lx\n",__LINE__,actual);return 1;}}while(0)
static HRESULT command(struct pw_d3d9_session *s,struct pw_d3d9_object_ref d,unsigned method,unsigned a,unsigned b)
{struct pw_d3d9_command q={.method=method,.args={a,b}};return pw_d3d9_session_command(s,d,&q);}
int wmain(int argc,WCHAR **argv)
{
 struct pw_d3d9_session *s=NULL;struct pw_d3d9_object_ref f,d,block;struct pw_d3d9_stateblock_reply reply;struct pw_d3d9_stateblock_request q;struct pw_d3d9_device_reply dr;
 if(argc!=3)return 2;
 for(unsigned cycle=0;cycle<3;cycle++){
  CHECK(pw_d3d9_session_open(argv[1],argv[2],&s));CHECK(pw_d3d9_session_create(s,D3D_SDK_VERSION,&f));
  struct pw_d3d9_device_request dq={.operation=1,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING,.focus_window={1,1,1},.parameters={.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.count=1,.swap_effect=D3DSWAPEFFECT_DISCARD,.windowed=1,.interval=D3DPRESENT_INTERVAL_IMMEDIATE,.window={1,1,1}}};
  CHECK(pw_d3d9_session_device(s,f,&dq,&dr));d=dr.object;
  q=(struct pw_d3d9_stateblock_request){PW_D3D9_SB_BEGIN,0};CHECK(pw_d3d9_session_stateblock(s,d,&q,&reply));
  CHECK(command(s,d,92,0,0));CHECK(command(s,d,57,D3DRS_ZENABLE,FALSE));
  q.method=PW_D3D9_SB_END;CHECK(pw_d3d9_session_stateblock(s,d,&q,&reply));block=reply.object;
  CHECK(command(s,d,57,D3DRS_ZENABLE,FALSE));q.method=PW_D3D9_SB_CAPTURE;CHECK(pw_d3d9_session_stateblock(s,block,&q,&reply));
  CHECK(command(s,d,57,D3DRS_ZENABLE,TRUE));q.method=PW_D3D9_SB_APPLY;CHECK(pw_d3d9_session_stateblock(s,block,&q,&reply));
  struct pw_d3d9_getter_request g={.method=58,.args={D3DRS_ZENABLE}};struct pw_d3d9_getter_reply gr;
  CHECK(pw_d3d9_session_getter(s,d,&g,&gr));if(gr.bytes!=4||gr.data.words[0]!=FALSE)return 3;
  CHECK(pw_d3d9_session_release(s,block));CHECK(pw_d3d9_session_release(s,d));CHECK(pw_d3d9_session_release(s,f));CHECK(pw_d3d9_session_close(s));s=NULL;
  printf("PW_STATEBLOCK_TRANSPORT cycle=%u status=0 restored=1\n",cycle);fflush(stdout);
 }
 return 0;
}
