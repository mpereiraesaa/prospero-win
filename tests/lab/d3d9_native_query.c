/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_query.h"
static struct pw_d3d9_query_reply call(struct pw_d3d9_native_query *p,struct pw_d3d9_query_request *q)
{
 unsigned char wire[PW_D3D9_QUERY_WIRE_MAX];size_t n;struct pw_d3d9_query_request decoded;struct pw_d3d9_query_reply r,result;
 assert(!pw_d3d9_query_request_encode(wire,sizeof(wire),&n,q));assert(!pw_d3d9_query_request_decode(&decoded,wire,n));pw_d3d9_native_query_call(p,&decoded,&r);
 assert(!pw_d3d9_query_reply_encode(wire,sizeof(wire),&n,q,&r));assert(!pw_d3d9_query_reply_decode(&result,q,wire,n));return result;
}
static void check(IDirect3DDevice9 *device,unsigned type)
{
 struct pw_d3d9_query_request q={.method=PW_D3D9_QUERY_CREATE,.type=type};struct pw_d3d9_query_reply r;struct pw_d3d9_native_query *p=NULL;HRESULT direct;
 direct=IDirect3DDevice9_CreateQuery(device,type,NULL);pw_d3d9_native_query_create(device,&q,&r,&p);assert(r.hresult==(uint32_t)direct&&!p);
 if(FAILED(direct)){printf("PW_QUERY type=%u unsupported=%08lx\n",type,(unsigned long)direct);return;}
 q.want_object=1;pw_d3d9_native_query_create(device,&q,&r,&p);assert(r.hresult==S_OK && p && r.type==type && r.size==pw_d3d9_query_type_size(type) && pw_d3d9_native_query_identity(p));
 q=(struct pw_d3d9_query_request){.method=PW_D3D9_QUERY_ISSUE,.flags=D3DISSUE_BEGIN};r=call(p,&q);assert(r.hresult==S_OK);
 if(type==D3DQUERYTYPE_OCCLUSION || type==D3DQUERYTYPE_TIMESTAMPDISJOINT){
  q=(struct pw_d3d9_query_request){.method=PW_D3D9_QUERY_DATA,.has_data=1,.size=4};memset(q.data,0xa5,4);r=call(p,&q);assert(r.hresult==S_FALSE && !memcmp(r.data,q.data,4));
 }
 q=(struct pw_d3d9_query_request){.method=PW_D3D9_QUERY_ISSUE,.flags=D3DISSUE_END};r=call(p,&q);assert(r.hresult==S_OK);
 q=(struct pw_d3d9_query_request){.method=PW_D3D9_QUERY_DATA,.flags=D3DGETDATA_FLUSH};
 for(unsigned n=0;;n++){r=call(p,&q);if(r.hresult!=S_FALSE)break;assert(n<10000);Sleep(1);}assert(r.hresult==S_OK&&!r.count);
 q.has_data=1;q.size=pw_d3d9_query_type_size(type);memset(q.data,0xa5,q.size);r=call(p,&q);assert(r.hresult==S_OK && r.count==q.size);
 if(type==D3DQUERYTYPE_EVENT){assert(r.data[0]==1);memset(q.data,0xa5,q.size);r=call(p,&q);assert(r.hresult==S_OK && r.data[0]==1 && r.data[1]==0xa5 && r.data[2]==0xa5 && r.data[3]==0xa5);}
 q.size=0;r=call(p,&q);assert(r.hresult==S_OK&&!r.count);
 q.size=16;memset(q.data,0xa5,16);r=call(p,&q);if(type!=D3DQUERYTYPE_VCACHE)assert(r.hresult==(uint32_t)D3DERR_INVALIDCALL && !memcmp(r.data,q.data,16));
 pw_d3d9_native_query_destroy(p);printf("PW_QUERY type=%u pass=1\n",type);
}
int wmain(int argc,WCHAR **argv)
{
 HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;IDirect3DDevice9 *device=NULL;HWND window;D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};
 if(argc!=2)return 2;
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_NATIVE_QUERY";assert(RegisterClassW(&cls));
 window=CreateWindowW(cls.lpszClassName,L"query",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 check(device,4);for(unsigned type=8;type<=12;type++)check(device,type);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);puts("PW_NATIVE_QUERY PASS");return 0;
}
