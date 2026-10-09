/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_up.h"
#include "../../wine/ps5/d3d9/pw_d3d9_kinds.h"
/* Isolated registry fixture: the local device context is the actual backend
 * COM interface. Production resolves its native window-owning context. */
struct pw_d3d9_native_device;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *device){return device;}
static void registry_proof(IDirect3DDevice9 *device)
{
 struct pw_d3d9_object_slot slots[4];struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent;
 assert(pw_d3d9_objects_init(&objects,slots,4,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 struct vertex {float x,y,z,w;DWORD color;};
 const struct vertex data[]={{0,0,0,1,0xffff0000},{64,0,0,1,0xffff0000},{32,64,0,1,0xffff0000}};
 assert(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW|D3DFVF_DIFFUSE)==S_OK);
 assert(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE)==S_OK);
 struct pw_d3d9_up_request q={.operation=PW_D3D9_UP_BEGIN,.draw={.method=83,.primitive_type=D3DPT_TRIANGLELIST,.primitive_count=1,.stride=sizeof(struct vertex)}};
 assert(pw_d3d9_up_measure(&q.draw)==PW_D3D9_UP_OK&&q.draw.vertex_bytes==sizeof(data));
 struct pw_d3d9_up_request begin=q;struct pw_d3d9_up_reply r;
 pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.transfer);uint64_t first=r.transfer;
 assert(slots[parent.id-1].queued_refs==1);
 pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&!r.transfer);
 q=(struct pw_d3d9_up_request){.operation=PW_D3D9_UP_COMMIT,.transfer=first};
 pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&!slots[parent.id-1].queued_refs);
 pw_d3d9_service_up_call(&objects,parent,&begin,&r);assert(r.hresult==S_OK&&r.transfer>first);uint64_t transfer=r.transfer;
 q=(struct pw_d3d9_up_request){.operation=PW_D3D9_UP_WRITE,.transfer=transfer,.count=sizeof(data)};memcpy(q.data,data,sizeof(data));
 pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==S_OK);
 assert(IDirect3DDevice9_BeginScene(device)==S_OK);
 q=(struct pw_d3d9_up_request){.operation=PW_D3D9_UP_COMMIT,.transfer=transfer};
 pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.transfer==transfer&&!slots[parent.id-1].queued_refs);
 assert(IDirect3DDevice9_EndScene(device)==S_OK);
 pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL);
 struct pw_d3d9_object_ref second;assert(pw_d3d9_object_reserve(&objects,&second));
 assert(pw_d3d9_object_commit(&objects,second,(uintptr_t)device+1,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 struct pw_d3d9_up_request maximum={.operation=PW_D3D9_UP_BEGIN,.draw={.method=83,.primitive_type=D3DPT_POINTLIST,.primitive_count=1048576,.stride=64}};
 assert(pw_d3d9_up_measure(&maximum.draw)==PW_D3D9_UP_OK&&maximum.draw.vertex_bytes==PW_D3D9_UP_LIMIT);
 pw_d3d9_service_up_call(&objects,parent,&maximum,&r);assert(r.hresult==S_OK);uint64_t maximum_transfer=r.transfer;
 pw_d3d9_service_up_call(&objects,second,&begin,&r);assert(r.hresult==(uint32_t)E_OUTOFMEMORY&&!slots[second.id-1].queued_refs);
 q=(struct pw_d3d9_up_request){.operation=PW_D3D9_UP_ABORT,.transfer=maximum_transfer};pw_d3d9_service_up_call(&objects,parent,&q,&r);assert(r.hresult==S_OK);
 assert(pw_d3d9_object_release(&objects,second));uintptr_t second_context;assert(pw_d3d9_object_take_destroy(&objects,second,&second_context));
 pw_d3d9_service_up_retire(&objects,second);assert(pw_d3d9_object_finish_destroy(&objects,second));
 pw_d3d9_service_up_call(&objects,parent,&begin,&r);assert(r.hresult==S_OK&&slots[parent.id-1].queued_refs==1);
 assert(pw_d3d9_object_release(&objects,parent));uintptr_t context;assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 pw_d3d9_service_up_shutdown(&objects);assert(pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 puts("PW_SERVICE_UP draw=1 aggregate_budget=1 consumed_commit=1 stale=1 cancelled_upload=1 parent_retained=1 status=0");
}
int wmain(int argc,WCHAR **argv)
{
 if(argc!=2)return 2;
 WNDCLASSW cls={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_PROGRAM_REGISTRY"};assert(RegisterClassW(&cls));
 HWND window=CreateWindowW(cls.lpszClassName,L"resource registry",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);
 IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);IDirect3DDevice9 *device=NULL;
 D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferFormat=D3DFMT_X8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=window};
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 for(unsigned i=0;i<3;i++)registry_proof(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);return 0;
}
