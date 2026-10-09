/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_service_resource.h"
#include "../../wine/ps5/d3d9/pw_d3d9_service_methods.h"
#include "../../wine/ps5/pw_d3d9_command_wire.h"
#include "../../wine/ps5/pw_d3d9_getter_wire.h"
#include "../../wine/ps5/pw_d3d9_bridge_wire.h"
/* Isolated registry fixture: the local device context is the actual backend
 * COM interface. Production resolves its native window-owning context. */
struct pw_d3d9_native_device;
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *device){return device;}
static void registry_proof(IDirect3DDevice9 *device)
{
 struct pw_d3d9_object_slot slots[4];struct pw_d3d9_objects objects;struct pw_d3d9_object_ref parent;
 assert(pw_d3d9_objects_init(&objects,slots,4,1,17));assert(pw_d3d9_object_reserve(&objects,&parent));
 assert(pw_d3d9_object_commit(&objects,parent,(uintptr_t)device,(uintptr_t)device,PW_D3D9_KIND_DEVICE));
 unsigned char input[8192],output[8192];size_t inbytes,outbytes;HRESULT hr;uint32_t method,hresult;
 struct pw_d3d9_command command={.method=57,.args={D3DRS_ZENABLE,D3DZB_FALSE}};
 assert(!pw_d3d9_command_encode(input,sizeof(input),&inbytes,&command));
 assert(pw_d3d9_service_methods(&objects,parent,PW_D3D9_COMMAND_CALL,input,inbytes,output,sizeof(output),&outbytes,&hr)&&hr==S_OK);
 assert(!pw_d3d9_command_reply_decode(&method,&hresult,output,outbytes)&&method==57&&hresult==S_OK);
 struct pw_d3d9_getter_request getter={.method=58,.args={D3DRS_ZENABLE}};struct pw_d3d9_getter_reply reply;
 assert(!pw_d3d9_getter_encode(input,sizeof(input),&inbytes,&getter));
 assert(pw_d3d9_service_methods(&objects,parent,PW_D3D9_GETTER_CALL,input,inbytes,output,sizeof(output),&outbytes,&hr)&&hr==S_OK);
 assert(!pw_d3d9_getter_reply_decode(&reply,&getter,output,outbytes)&&reply.data.words[0]==D3DZB_FALSE);
 assert(!pw_d3d9_service_methods(&objects,parent,PW_D3D9_GETTER_CALL,input,inbytes-1,output,sizeof(output),&outbytes,&hr));
 struct pw_d3d9_resource_request create={.operation=PW_D3D9_RESOURCE_CREATE_VB,.length=16384,.pool=D3DPOOL_MANAGED,.format_fvf=D3DFVF_XYZ};
 struct pw_d3d9_resource_reply resource;pw_d3d9_service_resource_call(&objects,parent,&create,&resource);assert(resource.hresult==S_OK);
 struct pw_d3d9_object_ref buffer=resource.object;
 command=(struct pw_d3d9_command){.method=100,.args={0,buffer.id,buffer.generation,0,12}};
 assert(!pw_d3d9_command_encode(input,sizeof(input),&inbytes,&command));
 assert(pw_d3d9_service_methods(&objects,parent,PW_D3D9_COMMAND_CALL,input,inbytes,output,sizeof(output),&outbytes,&hr)&&hr==S_OK);
 IDirect3DVertexBuffer9 *bound=NULL;UINT offset,stride;assert(IDirect3DDevice9_GetStreamSource(device,0,&bound,&offset,&stride)==S_OK&&bound&&stride==12);IDirect3DVertexBuffer9_Release(bound);
 assert(pw_d3d9_object_release(&objects,buffer));uintptr_t context;assert(pw_d3d9_object_take_destroy(&objects,buffer,&context));
 assert(pw_d3d9_service_resource_destroy(&objects,context)==S_OK);assert(pw_d3d9_object_finish_destroy(&objects,buffer));
 assert(pw_d3d9_service_methods(&objects,parent,PW_D3D9_COMMAND_CALL,input,inbytes,output,sizeof(output),&outbytes,&hr)&&hr==D3DERR_INVALIDCALL);
 assert(IDirect3DDevice9_SetStreamSource(device,0,NULL,0,0)==S_OK);
 assert(pw_d3d9_object_release(&objects,parent));assert(pw_d3d9_object_take_destroy(&objects,parent,&context));assert(pw_d3d9_object_finish_destroy(&objects,parent));
 assert(pw_d3d9_service_methods(&objects,parent,PW_D3D9_COMMAND_CALL,input,inbytes,output,sizeof(output),&outbytes,&hr)&&hr==D3DERR_INVALIDCALL&&!outbytes);
 puts("PW_SERVICE_METHODS state_roundtrip=1 owned_binding=1 stale_buffer=1 stale_device=1 malformed=1 status=0");
}
int wmain(int argc,WCHAR **argv)
{
 if(argc!=2)return 2;
 WNDCLASSW cls={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW_METHOD_REGISTRY"};assert(RegisterClassW(&cls));
 HWND window=CreateWindowW(cls.lpszClassName,L"resource registry",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);
 IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);IDirect3DDevice9 *device=NULL;
 D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferFormat=D3DFMT_X8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=window};
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 for(unsigned i=0;i<3;i++)registry_proof(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);return 0;
}
