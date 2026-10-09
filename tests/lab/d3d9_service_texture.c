/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_texture.h"
/* Isolated registry fixture: the local device context is the actual backend
 * COM interface. Production resolves its native window-owning context. */
struct pw_d3d9_native_device;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *device){return device;}
static void retire(struct pw_d3d9_objects *objects,struct pw_d3d9_object_ref ref)
{
 uintptr_t context;assert(pw_d3d9_object_release(objects,ref));
 assert(pw_d3d9_object_take_destroy(objects,ref,&context));
 assert(pw_d3d9_service_texture_destroy(objects,context)==S_OK);
 assert(pw_d3d9_object_finish_destroy(objects,ref));
}
static void registry_proof(IDirect3DDevice9 *device)
{
 struct pw_d3d9_object_slot slots[8];struct pw_d3d9_objects objects;
 struct pw_d3d9_object_ref parent,texture,surface,duplicate;
 assert(pw_d3d9_objects_init(&objects,slots,8,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_CREATE,.width=64,.height=64,.levels=1,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_MANAGED};
 struct pw_d3d9_texture_reply r;pw_d3d9_service_texture_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.object.id);texture=r.object;
 assert(slots[parent.id-1].queued_refs==1);
 void *owned=NULL;assert(pw_d3d9_service_texture_acquire(&objects,texture,PW_D3D9_KIND_SURFACE,device,&owned)==D3DERR_INVALIDCALL&&!owned);
 assert(pw_d3d9_service_texture_acquire(&objects,texture,PW_D3D9_KIND_TEXTURE_2D,(char *)device+1,&owned)==D3DERR_INVALIDCALL&&!owned);
 assert(pw_d3d9_service_texture_acquire(&objects,texture,PW_D3D9_KIND_TEXTURE_2D,device,&owned)==S_OK&&owned);
 assert(pw_d3d9_service_texture_adopt(&objects,parent,5,owned,&duplicate)==S_OK&&duplicate.id==texture.id);
 assert(pw_d3d9_object_release(&objects,duplicate));
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_SURFACE_LEVEL};
 pw_d3d9_service_texture_call(&objects,texture,&q,&r);assert(r.hresult==S_OK&&r.object.id);surface=r.object;
 assert(slots[parent.id-1].queued_refs==2);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CONTAINER,.value=PW_D3D9_CONTAINER_TEXTURE_2D};
 pw_d3d9_service_texture_call(&objects,surface,&q,&r);assert(r.hresult==S_OK&&r.container_kind==5&&r.object.id==texture.id&&r.levels==1);
 assert(pw_d3d9_object_release(&objects,r.object));
 q.value=PW_D3D9_CONTAINER_DEVICE;pw_d3d9_service_texture_call(&objects,surface,&q,&r);assert(r.hresult==(uint32_t)E_NOINTERFACE&&!r.object.id);

 struct pw_d3d9_texture_request create_surface={.operation=PW_D3D9_TEXTURE_CREATE_SURFACE,.width=8,.height=8,.format=D3DFMT_A8R8G8B8,.pool=D3DPOOL_SYSTEMMEM};
 pw_d3d9_service_texture_call(&objects,parent,&create_surface,&r);assert(r.hresult==S_OK);struct pw_d3d9_object_ref offscreen=r.object;
 assert(pw_d3d9_object_release(&objects,parent));uintptr_t context;assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_LOCK,.flags=D3DLOCK_READONLY};
 pw_d3d9_service_texture_call(&objects,surface,&q,&r);assert(r.hresult==S_OK&&r.length>=16384);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_READ,.lock_generation=r.lock_generation,.count=4096};
 pw_d3d9_service_texture_call(&objects,surface,&q,&r);assert(r.hresult==S_OK&&r.count==4096);
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_CONTAINER,.value=PW_D3D9_CONTAINER_DEVICE};
 pw_d3d9_service_texture_call(&objects,offscreen,&q,&r);assert(r.hresult==S_OK&&r.container_kind==2&&r.object.id==parent.id);
 assert(pw_d3d9_object_release(&objects,r.object));retire(&objects,offscreen);
 retire(&objects,texture);assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 retire(&objects,surface);
 assert(pw_d3d9_object_take_destroy(&objects,parent,&context)&&context==(uintptr_t)device);assert(pw_d3d9_object_finish_destroy(&objects,parent));
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_DESC};pw_d3d9_service_texture_call(&objects,surface,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL);
 puts("PW_SERVICE_TEXTURE parent_retained=1 active_lock_cancelled=1 stale_rejected=1 adopted_identity=1 status=0");
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
