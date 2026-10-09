/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_query.c"
#include "../../wine/ps5/d3d9/pw_d3d9_service_query.c"
/* Real backend COM device stands in for the production local window context. */
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *device){return device;}
static void check(IDirect3DDevice9 *device)
{
 struct pw_d3d9_object_slot slots[2];struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent,ref,alias;uintptr_t context;
 struct pw_d3d9_query_request q={.method=PW_D3D9_QUERY_CREATE,.type=D3DQUERYTYPE_EVENT};struct pw_d3d9_query_reply r,decoded;unsigned char wire[8192];size_t bytes=0;
 assert(pw_d3d9_objects_init(&objects,slots,2,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 pw_d3d9_service_query_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&!r.object.id&&!slots[parent.id-1].queued_refs);
 q.want_object=1;pw_d3d9_service_query_call(&objects,parent,&q,&r);assert(r.hresult==S_OK&&r.object.id&&r.size==4&&r.type==D3DQUERYTYPE_EVENT);ref=r.object;assert(slots[parent.id-1].queued_refs==1);
 /* Clone owned native references only to exercise canonical publication with
  * the same real Query9 identity, without fabricating a handle or COM object. */
 struct query_owner *owner=(void *)slots[ref.id-1].context;struct pw_d3d9_native_query *clone=HeapAlloc(GetProcessHeap(),0,sizeof(*clone));assert(clone);*clone=*owner->native;IDirect3DQuery9_AddRef(clone->object);IUnknown_AddRef(clone->identity);IDirect3DDevice9_AddRef(clone->parent);
 assert(publish_query(&objects,parent,clone,&alias)==S_OK&&alias.id==ref.id&&alias.generation==ref.generation&&slots[ref.id-1].guest_refs==2&&slots[parent.id-1].queued_refs==1);assert(pw_d3d9_object_release(&objects,alias));
 pw_d3d9_service_query_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)E_OUTOFMEMORY&&!r.object.id&&!r.type&&!r.size&&slots[parent.id-1].queued_refs==1);
 assert(!pw_d3d9_query_reply_encode(wire,sizeof(wire),&bytes,&q,&r)&&bytes==32);
 q=(struct pw_d3d9_query_request){.method=PW_D3D9_QUERY_ISSUE,.flags=D3DISSUE_END};pw_d3d9_service_query_call(&objects,ref,&q,&r);assert(r.hresult==S_OK);
 q=(struct pw_d3d9_query_request){.method=PW_D3D9_QUERY_DATA,.has_data=1,.size=4,.flags=D3DGETDATA_FLUSH};memset(q.data,0xa5,4);
 for(unsigned i=0;;i++){pw_d3d9_service_query_call(&objects,ref,&q,&r);assert(!pw_d3d9_query_reply_encode(wire,sizeof(wire),&bytes,&q,&r)&&bytes==36);assert(!pw_d3d9_query_reply_decode(&decoded,&q,wire,bytes));if(r.hresult!=S_FALSE)break;assert(i<10000);Sleep(1);}assert(r.hresult==S_OK&&r.data[0]==1);
 q.size=8;memset(q.data,0x5a,8);pw_d3d9_service_query_call(&objects,ref,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&r.count==8&&!memcmp(q.data,r.data,8));assert(!pw_d3d9_query_reply_encode(wire,sizeof(wire),&bytes,&q,&r));
 pw_d3d9_service_query_call(&objects,parent,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&r.count==8&&!memcmp(q.data,r.data,8));
 assert(pw_d3d9_object_release(&objects,parent));assert(!pw_d3d9_object_take_destroy(&objects,parent,&context));
 q.size=4;pw_d3d9_service_query_call(&objects,ref,&q,&r);assert(r.hresult==S_OK);
 pw_d3d9_objects_cancel(&objects);assert(pw_d3d9_object_take_destroy(&objects,ref,&context));assert(pw_d3d9_service_query_destroy(&objects,context)==S_OK);assert(pw_d3d9_object_finish_destroy(&objects,ref));assert(pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 pw_d3d9_service_query_call(&objects,ref,&q,&r);assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL&&r.count==4);puts("PW_SERVICE_QUERY PASS");
}
int wmain(int argc,WCHAR **argv)
{
 HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;IDirect3DDevice9 *device=NULL;HWND window;D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};
 if(argc!=2)return 2;
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_SERVICE_QUERY";assert(RegisterClassW(&cls));
 window=CreateWindowW(cls.lpszClassName,L"query",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 check(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);puts("PW_SERVICE_QUERY PASS");return 0;
}
