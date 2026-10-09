/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_resource.h"
/* Isolated registry fixture: the local device context is the actual backend
 * COM interface. Production resolves its native window-owning context. */
struct pw_d3d9_native_device;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *device){return device;}
static void registry_proof(IDirect3DDevice9 *device)
{
 struct pw_d3d9_object_slot slots[4];struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent,buffer;
 assert(pw_d3d9_objects_init(&objects,slots,4,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 struct pw_d3d9_resource_request q={.operation=PW_D3D9_RESOURCE_CREATE_VB,.length=16384,.pool=D3DPOOL_MANAGED,.format_fvf=D3DFVF_XYZ};
 struct pw_d3d9_resource_reply r;pw_d3d9_service_resource_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.object.id);buffer=r.object;
 assert(slots[parent.id-1].queued_refs==1);
 void *owned=NULL;assert(pw_d3d9_service_resource_acquire(&objects,buffer,PW_D3D9_KIND_INDEX_BUFFER,device,&owned)==D3DERR_INVALIDCALL&&!owned);
 assert(pw_d3d9_service_resource_acquire(&objects,buffer,PW_D3D9_KIND_VERTEX_BUFFER,(char *)device+1,&owned)==D3DERR_INVALIDCALL&&!owned);
 assert(pw_d3d9_service_resource_acquire(&objects,buffer,PW_D3D9_KIND_VERTEX_BUFFER,device,&owned)==S_OK&&owned);struct pw_d3d9_object_ref duplicate;assert(pw_d3d9_service_resource_adopt(&objects,parent,3,owned,&duplicate)==S_OK&&duplicate.id==buffer.id);assert(pw_d3d9_object_release(&objects,duplicate));
 assert(pw_d3d9_object_release(&objects,parent));uintptr_t context;assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_LOCK,.flags=D3DLOCK_READONLY};
 pw_d3d9_service_resource_call(&objects,buffer,&q,&r);assert(r.hresult==S_OK&&r.length==16384);
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_READ,.lock_generation=r.lock_generation,.count=4096};
 pw_d3d9_service_resource_call(&objects,buffer,&q,&r);assert(r.hresult==S_OK&&r.count==4096);
 unsigned char wire[PW_D3D9_RESOURCE_MAX_WIRE];size_t bytes;assert(!pw_d3d9_resource_reply_encode(wire,sizeof(wire),&bytes,&r)&&bytes>4096);
 pw_d3d9_objects_cancel(&objects);
 assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_take_destroy(&objects,buffer,&context));
 assert(pw_d3d9_service_resource_destroy(&objects,context)==S_OK);assert(pw_d3d9_object_finish_destroy(&objects,buffer));
 assert(pw_d3d9_object_take_destroy(&objects,parent,&context)&&context==(uintptr_t)device);assert(pw_d3d9_object_finish_destroy(&objects,parent));
 q=(struct pw_d3d9_resource_request){.operation=PW_D3D9_RESOURCE_DESC};pw_d3d9_service_resource_call(&objects,buffer,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL);
 puts("PW_SERVICE_RESOURCE parent_retained=1 active_lock_cancelled=1 stale_rejected=1 copied=4096 status=0");
}
int wmain(int argc,WCHAR **argv)
{
 if(argc!=2)return 2;
 WNDCLASSW cls={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_RESOURCE_REGISTRY"};assert(RegisterClassW(&cls));
 HWND window=CreateWindowW(cls.lpszClassName,L"resource registry",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);
 IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);IDirect3DDevice9 *device=NULL;
 D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferFormat=D3DFMT_X8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=window};
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 for(unsigned i=0;i<3;i++)registry_proof(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);return 0;
}
