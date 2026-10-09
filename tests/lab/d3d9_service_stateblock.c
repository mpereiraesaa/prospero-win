/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_stateblock.h"
#include "../../wine/ps5/d3d9/pw_d3d9_kinds.h"
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
#include "../../wine/ps5/d3d9/pw_d3d9_native_draw_state.h"
static unsigned recording=PW_D3D9_RECORDING_LIVE;
void pw_d3d9_native_device_recording_outcome(struct pw_d3d9_native_device *d,uint32_t method,uint32_t hr)
{assert(d);pw_d3d9_native_recording_outcome(&recording,method,hr);}
#endif
/* Isolated registry fixture: the local device context is the actual backend
 * COM interface. Production resolves its native window-owning context. */
struct pw_d3d9_native_device;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *device){return device;}
static void registry_proof(IDirect3DDevice9 *device)
{
 unsigned char scratch[8192];size_t bytes=99;
 struct pw_d3d9_stateblock_request encode_request={.method=PW_D3D9_SB_BEGIN};
 struct pw_d3d9_stateblock_reply encode_reply={.hresult=S_OK},decoded;
 memset(scratch,0xa5,sizeof(scratch));
 assert(pw_d3d9_service_stateblock_reply(scratch,15,&bytes,&encode_request,&encode_reply)==PW_D3D9_SB_INVALID&&bytes==99&&scratch[0]==0xa5);
 assert(pw_d3d9_service_stateblock_reply(scratch,sizeof(scratch),&bytes,&encode_request,&encode_reply)==PW_D3D9_SB_OK&&bytes==16&&scratch[16]==0xa5);
 assert(pw_d3d9_stateblock_reply_decode(&decoded,&encode_request,scratch,bytes)==PW_D3D9_SB_OK&&decoded.hresult==S_OK);

 struct pw_d3d9_object_slot slots[4];struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent;
 assert(pw_d3d9_objects_init(&objects,slots,4,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 assert(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE)==S_OK);
 struct pw_d3d9_stateblock_request q={PW_D3D9_SB_CREATE,D3DSBT_ALL};struct pw_d3d9_stateblock_reply r;
 pw_d3d9_service_stateblock_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.object.id);struct pw_d3d9_object_ref block=r.object;
 assert(slots[parent.id-1].queued_refs==1);assert(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_CW)==S_OK);
 q=(struct pw_d3d9_stateblock_request){PW_D3D9_SB_APPLY,0};pw_d3d9_service_stateblock_call(&objects,block,&q,&r);assert(r.hresult==S_OK&&!r.object.id);
 DWORD value;assert(IDirect3DDevice9_GetRenderState(device,D3DRS_CULLMODE,&value)==S_OK&&value==D3DCULL_NONE);
 assert(pw_d3d9_object_release(&objects,parent));uintptr_t context;assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 q.method=PW_D3D9_SB_CAPTURE;pw_d3d9_service_stateblock_call(&objects,block,&q,&r);assert(r.hresult==S_OK);
 pw_d3d9_objects_cancel(&objects);assert(pw_d3d9_object_take_destroy(&objects,block,&context));assert(pw_d3d9_service_stateblock_destroy(&objects,context)==S_OK);assert(pw_d3d9_object_finish_destroy(&objects,block));
 assert(pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 pw_d3d9_service_stateblock_call(&objects,block,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL);
 puts("PW_SERVICE_STATEBLOCK scratch8192=1 created=1 restored=1 parent_retained=1 stale=1 status=0");
}
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
static void recording_proof(IDirect3DDevice9 *device)
{
 struct pw_d3d9_object_slot slot;struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent;
 assert(pw_d3d9_objects_init(&objects,&slot,1,1,19));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 struct pw_d3d9_stateblock_request q={PW_D3D9_SB_BEGIN,0};struct pw_d3d9_stateblock_reply r;
 recording=PW_D3D9_RECORDING_LIVE;
 pw_d3d9_service_stateblock_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&recording==PW_D3D9_RECORDING_ACTIVE);
 pw_d3d9_service_stateblock_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&recording==PW_D3D9_RECORDING_ACTIVE);
 q.method=PW_D3D9_SB_END;
 pw_d3d9_service_stateblock_call(&objects,parent,&q,&r);
 assert(r.hresult==(uint32_t)E_OUTOFMEMORY&&!r.object.id&&recording==PW_D3D9_RECORDING_LIVE&&!slot.queued_refs);
 pw_d3d9_service_stateblock_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&recording==PW_D3D9_RECORDING_LIVE);
 unsigned state=PW_D3D9_RECORDING_ACTIVE;
 pw_d3d9_native_recording_outcome(&state,61,S_FALSE);assert(state==PW_D3D9_RECORDING_UNKNOWN);
 pw_d3d9_native_recording_outcome(&state,60,D3DERR_INVALIDCALL);assert(state==PW_D3D9_RECORDING_UNKNOWN);
 uintptr_t dead;assert(pw_d3d9_object_release(&objects,parent));assert(pw_d3d9_object_take_destroy(&objects,parent,&dead));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 puts("PW_NATIVE_RECORDING begin=1 failed_begin=1 end_publish_failure=1 failed_end=1 unexpected_success=1 status=0");
}
#endif
int wmain(int argc,WCHAR **argv)
{
 if(argc!=2)return 2;
 WNDCLASSW cls={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_STATEBLOCK_REGISTRY"};assert(RegisterClassW(&cls));
 HWND window=CreateWindowW(cls.lpszClassName,L"resource registry",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);
 IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);IDirect3DDevice9 *device=NULL;
 D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferFormat=D3DFMT_X8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=window};
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 for(unsigned i=0;i<3;i++){
  registry_proof(device);
#ifdef PW_D3D9_ENABLE_DRAW_BATCH
  recording_proof(device);
#endif
 }
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);return 0;
}
