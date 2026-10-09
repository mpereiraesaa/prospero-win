/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_program.h"
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
 const unsigned char declaration[]={0,0,0,0,2,0,0,0,255,0,0,0,17,0,0,0};
 struct pw_d3d9_program_request q={.operation=PW_D3D9_PROGRAM_BEGIN,.kind=7,.total=sizeof(declaration)};
 struct pw_d3d9_program_reply r;pw_d3d9_service_program_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.transfer);
 uint64_t transfer=r.transfer;assert(slots[parent.id-1].queued_refs==1);
 pw_d3d9_service_program_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&!r.transfer);
 q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_WRITE,.kind=7,.transfer=transfer,.count=sizeof(declaration)};memcpy(q.data,declaration,sizeof(declaration));
 pw_d3d9_service_program_call(&objects,parent,&q,&r);assert(r.hresult==S_OK);
 q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_COMMIT,.kind=7,.transfer=transfer};
 pw_d3d9_service_program_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.id&&r.generation);
 struct pw_d3d9_object_ref program={r.id,r.generation};assert(slots[parent.id-1].queued_refs==1);
 void *owned=NULL;assert(pw_d3d9_service_program_acquire(&objects,program,8,device,&owned)==D3DERR_INVALIDCALL&&!owned);
 assert(pw_d3d9_service_program_acquire(&objects,program,7,device,&owned)==S_OK&&owned);
 D3DVERTEXELEMENT9 copied[3];UINT count=3;assert(IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)owned,copied,&count)==S_OK&&count==2);IUnknown_Release((IUnknown *)owned);
 pw_d3d9_service_program_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&!r.id&&!r.transfer);
 q=(struct pw_d3d9_program_request){.operation=PW_D3D9_PROGRAM_BEGIN,.kind=7,.total=sizeof(declaration)};
 pw_d3d9_service_program_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.transfer>transfer);
 assert(slots[parent.id-1].queued_refs==2);assert(pw_d3d9_object_release(&objects,parent));
 pw_d3d9_service_program_shutdown(&objects);assert(slots[parent.id-1].queued_refs==1);
 pw_d3d9_objects_cancel(&objects);uintptr_t context;assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 assert(pw_d3d9_object_take_destroy(&objects,program,&context));assert(pw_d3d9_service_program_destroy(&objects,context)==S_OK);assert(pw_d3d9_object_finish_destroy(&objects,program));
 assert(pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 puts("PW_SERVICE_PROGRAM created=1 parent_retained=1 generation=1 stale=1 cancelled_upload=1 status=0");
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
