/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include "../../wine/ps5/d3d9/pw_d3d9_gamma_proxy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pw_d3d9_session *session;static struct pw_d3d9_object_ref device;static IDirect3DDevice9 parent;static LONG refs=1;
static ULONG WINAPI addref(IDirect3DDevice9 *p){assert(p==&parent);return InterlockedIncrement(&refs);}
static ULONG WINAPI release(IDirect3DDevice9 *p){assert(p==&parent);return InterlockedDecrement(&refs);}
static HRESULT call(IDirect3DDevice9 *p,const struct pw_d3d9_gamma_request *q,struct pw_d3d9_gamma_reply *r)
{assert(p==&parent);return pw_d3d9_session_gamma(session,device,q,r);}
static void fail(IDirect3DDevice9 *p,HRESULT hr){(void)p;fprintf(stderr,"gamma transport failure %08lx\n",(unsigned long)hr);assert(0);}
int wmain(int argc,WCHAR **argv)
{
 if(argc!=3)return 2;
 IDirect3DDevice9Vtbl table={0};struct pw_d3d9_gamma_proxy_ops ops={call,fail};table.AddRef=addref;table.Release=release;parent.lpVtbl=&table;pw_d3d9_gamma_proxy_install(&table,&ops);
 for(unsigned cycle=0;cycle<3;cycle++){
  struct pw_d3d9_object_ref factory;struct pw_d3d9_device_reply reply;D3DGAMMARAMP original,ramp,read;
  assert(pw_d3d9_session_open(argv[1],argv[2],&session)==S_OK);assert(pw_d3d9_session_create(session,D3D_SDK_VERSION,&factory)==S_OK);
  struct pw_d3d9_device_request q={.operation=1,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING,.focus_window={1,1,1},.parameters={.width=64,.height=64,.format=D3DFMT_A8R8G8B8,.count=1,.swap_effect=D3DSWAPEFFECT_DISCARD,.windowed=1,.interval=D3DPRESENT_INTERVAL_IMMEDIATE,.window={1,1,1}}};
  assert(pw_d3d9_session_device(session,factory,&q,&reply)==S_OK);device=reply.object;
  IDirect3DDevice9_GetGammaRamp(&parent,0,&original);
  for(unsigned i=0;i<256;i++){ramp.red[i]=(WORD)(i*257);ramp.green[i]=(WORD)(i*i*65535u/65025u);ramp.blue[i]=(WORD)(i*257);}
  IDirect3DDevice9_SetGammaRamp(&parent,0,0xfedcba98u,&ramp);memset(&read,0xa5,sizeof(read));IDirect3DDevice9_GetGammaRamp(&parent,0,&read);assert(!memcmp(&read,&ramp,sizeof(read)));
  memset(&read,0xa5,sizeof(read));ramp=read;IDirect3DDevice9_GetGammaRamp(&parent,0xffffffffu,&read);assert(!memcmp(&read,&ramp,sizeof(read)));
  IDirect3DDevice9_SetGammaRamp(&parent,0,0,NULL);IDirect3DDevice9_GetGammaRamp(&parent,0,NULL);
  IDirect3DDevice9_SetGammaRamp(&parent,0,0,&original);IDirect3DDevice9_GetGammaRamp(&parent,0,&read);assert(!memcmp(&read,&original,sizeof(read))&&refs==1);
  assert(pw_d3d9_session_release(session,device)==S_OK);assert(pw_d3d9_session_release(session,factory)==S_OK);assert(pw_d3d9_session_close(session)==S_OK);session=NULL;
  printf("PW_GAMMA_TRANSPORT cycle=%u channels=1 noop=1 restored=1 status=0\n",cycle);fflush(stdout);
 }
 return 0;
}
