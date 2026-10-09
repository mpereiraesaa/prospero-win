/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
static void bind_depth(struct pw_d3d9_session *s,struct pw_d3d9_object_ref d,struct pw_d3d9_object_ref surface)
{struct pw_d3d9_command q={.method=39,.args={surface.id,surface.generation}};assert(pw_d3d9_session_command(s,d,&q)==S_OK);}
int wmain(int argc,WCHAR **argv)
{
 if(argc!=3)return 2;
 for(unsigned cycle=0;cycle<3;cycle++){
  struct pw_d3d9_session *s;struct pw_d3d9_object_ref factory,device,texture,surface,old;struct pw_d3d9_device_reply dr;
  assert(pw_d3d9_session_open(argv[1],argv[2],&s)==S_OK);assert(pw_d3d9_session_create(s,D3D_SDK_VERSION,&factory)==S_OK);
  struct pw_d3d9_device_request dq={.operation=1,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING,.focus_window={1,1,1},.parameters={.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.count=1,.swap_effect=D3DSWAPEFFECT_DISCARD,.windowed=1,.interval=D3DPRESENT_INTERVAL_IMMEDIATE,.window={1,1,1}}};
  assert(pw_d3d9_session_device(s,factory,&dq,&dr)==S_OK);device=dr.object;
  struct pw_d3d9_object_getter_request g={.method=40};struct pw_d3d9_object_getter_reply gr;
  assert(pw_d3d9_session_object_getter(s,device,&g,&gr)==S_OK&&gr.id&&gr.kind==6);old=(struct pw_d3d9_object_ref){gr.id,gr.generation};
  struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_CREATE,.width=64,.height=64,.levels=1,.usage=D3DUSAGE_DEPTHSTENCIL,.format=MAKEFOURCC('I','N','T','Z'),.pool=D3DPOOL_DEFAULT};struct pw_d3d9_texture_reply r;
  assert(pw_d3d9_session_texture(s,device,&q,&r)==S_OK&&r.object.id);texture=r.object;
  q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_SURFACE_LEVEL};assert(pw_d3d9_session_texture(s,texture,&q,&r)==S_OK&&r.object.id);surface=r.object;
  q.operation=PW_D3D9_TEXTURE_DESC;assert(pw_d3d9_session_texture(s,surface,&q,&r)==S_OK&&r.desc.format==MAKEFOURCC('I','N','T','Z')&&r.desc.width==64&&r.desc.height==64);
  bind_depth(s,device,surface);assert(pw_d3d9_session_object_getter(s,device,&g,&gr)==S_OK&&gr.id==surface.id&&gr.generation==surface.generation);assert(pw_d3d9_session_release(s,(struct pw_d3d9_object_ref){gr.id,gr.generation})==S_OK);
  struct pw_d3d9_command clear={.method=43,.args={0,D3DCLEAR_ZBUFFER,0,0x3f000000u,0}};assert(pw_d3d9_session_command(s,device,&clear)==S_OK);
  bind_depth(s,device,old);assert(pw_d3d9_session_object_getter(s,device,&g,&gr)==S_OK&&gr.id==old.id&&gr.generation==old.generation);assert(pw_d3d9_session_release(s,(struct pw_d3d9_object_ref){gr.id,gr.generation})==S_OK);
  assert(pw_d3d9_session_release(s,old)==S_OK);assert(pw_d3d9_session_release(s,surface)==S_OK);assert(pw_d3d9_session_release(s,texture)==S_OK);assert(pw_d3d9_session_release(s,device)==S_OK);assert(pw_d3d9_session_release(s,factory)==S_OK);assert(pw_d3d9_session_close(s)==S_OK);
  printf("PW_INTZ_TRANSPORT cycle=%u created=1 depth_used=1 restored=1 status=0\n",cycle);fflush(stdout);
 }
 return 0;
}
