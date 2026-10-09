/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_texture.h"
#include "../../wine/ps5/d3d9/pw_d3d9_service_object_getter.h"
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
 struct pw_d3d9_object_slot slots[8];struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent;
 assert(pw_d3d9_objects_init(&objects,slots,8,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 struct pw_d3d9_object_getter_request q={.method=38};struct pw_d3d9_object_getter_reply r;
 pw_d3d9_service_object_getter(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.kind==6&&r.id);
 struct pw_d3d9_object_ref surface={r.id,r.generation};
 pw_d3d9_service_object_getter(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.id==surface.id&&r.generation==surface.generation);
 assert(slots[parent.id-1].queued_refs==1);assert(pw_d3d9_object_release(&objects,surface));
 q.method=64;pw_d3d9_service_object_getter(&objects,parent,&q,&r);assert(r.hresult==S_OK&&!r.id&&!r.kind);
 assert(pw_d3d9_object_release(&objects,parent));uintptr_t context;assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 retire(&objects,surface);assert(pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 pw_d3d9_service_object_getter(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&!r.id);
 puts("PW_SERVICE_OBJECT_GETTER implicit_surface=1 canonical=1 null_texture=1 parent_retained=1 stale=1 status=0");
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
