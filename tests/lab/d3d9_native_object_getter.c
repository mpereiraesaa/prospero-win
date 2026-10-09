/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <assert.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_object_getter.h"
static struct pw_d3d9_native_object_result query(IDirect3DDevice9 *d,unsigned method,unsigned a,unsigned b,unsigned c,HRESULT expected)
{
 struct pw_d3d9_object_getter_request q={method,{a,b,c}},decoded;
 struct pw_d3d9_native_object_result r;unsigned char wire[24];size_t n;HRESULT hr;
 assert(!pw_d3d9_object_getter_encode(wire,sizeof(wire),&n,&q));
 assert(!pw_d3d9_object_getter_decode(&decoded,wire,n));
 hr=pw_d3d9_native_object_getter(d,&decoded,&r);assert(hr==expected);
 if(FAILED(hr))assert(!r.object&&!r.kind&&!r.offset&&!r.stride);
 printf("PW_NATIVE_OBJECT method=%u hr=%08x kind=%u\n",method,(unsigned)hr,r.kind);return r;
}
int wmain(int argc,WCHAR **argv)
{
 WNDCLASSW cls={0};HWND window;HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;
 IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS pp={0};struct pw_d3d9_native_object_result r,back;
 IDirect3DTexture9 *texture=NULL;IDirect3DVertexBuffer9 *vb=NULL;IDirect3DIndexBuffer9 *ib=NULL;
 IDirect3DVertexShader9 *vs=NULL;IDirect3DPixelShader9 *ps=NULL;IDirect3DSurface9 *depth=NULL;
 const DWORD vertex[]={0xfffe0101,1,0xc00f0000,0x90e40000,0xffff};
 const DWORD pixel[]={0xffff0200,0x02000001,0x800f0800,0xa0e40000,0xffff};
 HRESULT hr;if(argc!=2)return 2;
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_NATIVE_OBJECT";
 assert(RegisterClassW(&cls));window=CreateWindowW(cls.lpszClassName,L"objects",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 back=query(device,18,0,0,D3DBACKBUFFER_TYPE_MONO,S_OK);assert(back.object&&back.kind==6);
 r=query(device,38,0,0,0,S_OK);assert(r.object==back.object);pw_d3d9_native_object_result_release(&r);pw_d3d9_native_object_result_release(&back);
 hr=IDirect3DDevice9_GetDepthStencilSurface(device,&depth);if(depth)IDirect3DSurface9_Release(depth);
 r=query(device,40,0,0,0,hr);pw_d3d9_native_object_result_release(&r);
 r=query(device,64,0,0,0,S_OK);assert(!r.object&&!r.kind);
 assert(IDirect3DDevice9_CreateTexture(device,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL)==S_OK);
 assert(IDirect3DDevice9_SetTexture(device,0,(IDirect3DBaseTexture9 *)texture)==S_OK);
 r=query(device,64,0,0,0,S_OK);assert(r.object==(void *)texture&&r.kind==5);pw_d3d9_native_object_result_release(&r);
 assert(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZ)==S_OK);r=query(device,88,0,0,0,S_OK);assert(r.object&&r.kind==7);pw_d3d9_native_object_result_release(&r);
 assert(IDirect3DDevice9_CreateVertexShader(device,vertex,&vs)==S_OK);assert(IDirect3DDevice9_SetVertexShader(device,vs)==S_OK);
 r=query(device,93,0,0,0,S_OK);assert(r.object==(void *)vs&&r.kind==8);pw_d3d9_native_object_result_release(&r);
 assert(IDirect3DDevice9_CreatePixelShader(device,pixel,&ps)==S_OK);assert(IDirect3DDevice9_SetPixelShader(device,ps)==S_OK);
 r=query(device,108,0,0,0,S_OK);assert(r.object==(void *)ps&&r.kind==9);pw_d3d9_native_object_result_release(&r);
 assert(IDirect3DDevice9_CreateVertexBuffer(device,128,0,D3DFVF_XYZ,D3DPOOL_MANAGED,&vb,NULL)==S_OK);
 assert(IDirect3DDevice9_SetStreamSource(device,0,vb,4,12)==S_OK);IDirect3DVertexBuffer9_Release(vb);
 r=query(device,101,0,0,0,S_OK);assert(r.object==(void *)vb&&r.kind==3&&r.offset==4&&r.stride==12);pw_d3d9_native_object_result_release(&r);
 assert(IDirect3DDevice9_CreateIndexBuffer(device,32,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL)==S_OK);
 assert(IDirect3DDevice9_SetIndices(device,ib)==S_OK);IDirect3DIndexBuffer9_Release(ib);
 r=query(device,105,0,0,0,S_OK);assert(r.object==(void *)ib&&r.kind==4);pw_d3d9_native_object_result_release(&r);
 assert(IDirect3DDevice9_SetTexture(device,0,NULL)==S_OK);IDirect3DTexture9_Release(texture);
 assert(IDirect3DDevice9_SetVertexShader(device,NULL)==S_OK);IDirect3DVertexShader9_Release(vs);
 assert(IDirect3DDevice9_SetPixelShader(device,NULL)==S_OK);IDirect3DPixelShader9_Release(ps);
 assert(IDirect3DDevice9_SetStreamSource(device,0,NULL,0,0)==S_OK);assert(IDirect3DDevice9_SetIndices(device,NULL)==S_OK);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);
 puts("PW_NATIVE_OBJECT PASS");return 0;
}
