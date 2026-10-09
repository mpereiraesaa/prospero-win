/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_private_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct pw_d3d9_private_data store;
static GUID key={1,2,3,{4}},other={2,2,3,{4}};
static ULONG references=1;
static int reenter;
static HRESULT WINAPI query(IUnknown *p,REFIID i,void **o){(void)p;(void)i;(void)o;return E_NOINTERFACE;}
static ULONG WINAPI add(IUnknown *p)
{
 (void)p;ULONG n=++references;
 if(reenter){reenter=0;assert(SUCCEEDED(pw_d3d9_private_free(&store,&key)));}
 return n;
}
static ULONG WINAPI release(IUnknown *p)
{
 (void)p;DWORD n=0;assert(pw_d3d9_private_get(&store,&other,NULL,&n)==D3DERR_NOTFOUND);return --references;
}
#ifdef _WIN64
static void compare_resource(IDirect3DResource9 *resource,IUnknown *unknown)
{
 struct pw_d3d9_private_data local={0};DWORD n1=99,n2=99;unsigned char data[9]={1,2,3},a[9]={0},b[9]={0};HRESULT actual,ours;
 actual=IDirect3DResource9_GetPrivateData(resource,&key,a,&n1);ours=pw_d3d9_private_get(&local,&key,b,&n2);assert(actual==ours&&n1==0&&n2==0);
 assert(IDirect3DResource9_FreePrivateData(resource,&key)==pw_d3d9_private_free(&local,&key));
 actual=IDirect3DResource9_SetPrivateData(resource,&key,NULL,99,0);ours=pw_d3d9_private_set(&local,&key,NULL,99,0);assert(actual==ours&&actual==S_OK);
 actual=IDirect3DResource9_SetPrivateData(resource,&key,data,9,0x80000000u);ours=pw_d3d9_private_set(&local,&key,data,9,0x80000000u);assert(actual==ours&&actual==S_OK);
 n1=n2=0;actual=IDirect3DResource9_GetPrivateData(resource,&key,NULL,&n1);ours=pw_d3d9_private_get(&local,&key,NULL,&n2);assert(actual==ours&&actual==S_OK&&n1==9&&n2==9);
 n1=n2=8;actual=IDirect3DResource9_GetPrivateData(resource,&key,a,&n1);ours=pw_d3d9_private_get(&local,&key,b,&n2);assert(actual==ours&&actual==D3DERR_MOREDATA&&n1==n2&&!memcmp(a,b,9));
 n1=n2=9;assert(IDirect3DResource9_GetPrivateData(resource,&key,a,&n1)==pw_d3d9_private_get(&local,&key,b,&n2)&&!memcmp(a,b,9));
 assert(IDirect3DResource9_GetPrivateData(resource,&key,a,NULL)==pw_d3d9_private_get(&local,&key,b,NULL));
 assert(IDirect3DResource9_GetPrivateData(resource,&key,NULL,NULL)==pw_d3d9_private_get(&local,&key,NULL,NULL));
 actual=IDirect3DResource9_SetPrivateData(resource,&key,unknown,sizeof(void *)-1,D3DSPD_IUNKNOWN);ours=pw_d3d9_private_set(&local,&key,unknown,sizeof(void *)-1,D3DSPD_IUNKNOWN);assert(actual==ours&&actual==D3DERR_INVALIDCALL);
 assert(IDirect3DResource9_SetPrivateData(resource,&key,unknown,sizeof(void *),D3DSPD_IUNKNOWN)==pw_d3d9_private_set(&local,&key,unknown,sizeof(void *),D3DSPD_IUNKNOWN));
 IUnknown *u1=NULL,*u2=NULL;n1=n2=sizeof(void *);
 assert(IDirect3DResource9_GetPrivateData(resource,&key,&u1,&n1)==pw_d3d9_private_get(&local,&key,&u2,&n2)&&u1==unknown&&u2==unknown);IUnknown_Release(u1);IUnknown_Release(u2);
 assert(IDirect3DResource9_FreePrivateData(resource,&key)==pw_d3d9_private_free(&local,&key));
 pw_d3d9_private_dispose(&local);assert(references==1);
}
static void actual_backend(const char *path,IUnknown *unknown)
{
 WNDCLASSA cls={.lpfnWndProc=DefWindowProcA,.lpszClassName="PW_PRIVATE_DATA_NATIVE"};cls.hInstance=GetModuleHandleW(NULL);assert(RegisterClassA(&cls));
 HWND window=CreateWindowA(cls.lpszClassName,"metadata",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 HMODULE dll=LoadLibraryA(path);assert(dll);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(dll,"Direct3DCreate9");assert(factory);IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);
 IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS pp={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferWidth=64,.BackBufferHeight=64,.BackBufferFormat=D3DFMT_X8R8G8B8,.hDeviceWindow=window};
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 IDirect3DVertexBuffer9 *vb=NULL;IDirect3DIndexBuffer9 *ib=NULL;IDirect3DTexture9 *texture=NULL;IDirect3DSurface9 *surface=NULL;
 assert(IDirect3DDevice9_CreateVertexBuffer(device,256,0,0,D3DPOOL_MANAGED,&vb,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateIndexBuffer(device,256,0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateTexture(device,16,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL)==S_OK);
 assert(IDirect3DDevice9_CreateOffscreenPlainSurface(device,16,16,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&surface,NULL)==S_OK);
 compare_resource((IDirect3DResource9 *)vb,unknown);compare_resource((IDirect3DResource9 *)ib,unknown);compare_resource((IDirect3DResource9 *)texture,unknown);compare_resource((IDirect3DResource9 *)surface,unknown);
 IDirect3DSurface9_Release(surface);IDirect3DTexture9_Release(texture);IDirect3DIndexBuffer9_Release(ib);IDirect3DVertexBuffer9_Release(vb);IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);
 DestroyWindow(window);UnregisterClassA(cls.lpszClassName,cls.hInstance);FreeLibrary(dll);
 puts("PW_PRIVATE_NATIVE PASS vb=1 ib=1 texture=1 surface=1 edge_semantics=1");
}
#endif
int main(int argc,char **argv)
{
 IUnknownVtbl vtable={query,add,release};IUnknown object={&vtable};
 unsigned char source[9]={1,2,3,4,5},out[9]={0};DWORD n;IUnknown *got=NULL;
 assert(pw_d3d9_private_get(&store,&key,NULL,NULL)==D3DERR_NOTFOUND);
 n=9;assert(pw_d3d9_private_get(&store,&key,out,&n)==D3DERR_NOTFOUND&&n==0);
 assert(SUCCEEDED(pw_d3d9_private_set(&store,&key,source,9,0)));source[0]=99;
 n=0;assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,NULL,&n))&&n==9);
 n=8;assert(pw_d3d9_private_get(&store,&key,out,&n)==D3DERR_MOREDATA&&n==9&&!out[0]);
 n=9;assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,out,&n))&&out[0]==1);
 assert(pw_d3d9_private_set(&store,&key,&object,1,D3DSPD_IUNKNOWN)==D3DERR_INVALIDCALL&&references==1);
 assert(SUCCEEDED(pw_d3d9_private_set(&store,&key,&object,sizeof(void *),D3DSPD_IUNKNOWN))&&references==2);
 reenter=1;n=sizeof(got);assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,&got,&n))&&got==&object&&references==2);
 IUnknown_Release(got);assert(references==1);n=4;assert(pw_d3d9_private_get(&store,&key,NULL,&n)==D3DERR_NOTFOUND);
 assert(SUCCEEDED(pw_d3d9_private_set(&store,&key,NULL,sizeof(void *),D3DSPD_IUNKNOWN)));
 got=&object;n=sizeof(got);assert(SUCCEEDED(pw_d3d9_private_get(&store,&key,&got,&n))&&!got);
 assert(SUCCEEDED(pw_d3d9_private_free(&store,&key))&&SUCCEEDED(pw_d3d9_private_free(&store,&key)));
 for(unsigned i=0;i<PW_D3D9_PRIVATE_MAX_ENTRIES;i++){GUID k=key;k.Data1=i+10;assert(SUCCEEDED(pw_d3d9_private_set(&store,&k,source,1,0)));}
 assert(pw_d3d9_private_set(&store,&key,source,1,0)==E_OUTOFMEMORY);
 assert(pw_d3d9_private_set(&store,&key,source,PW_D3D9_PRIVATE_MAX_BYTES,0)==E_OUTOFMEMORY);
 pw_d3d9_private_dispose(&store);assert(!store.count);
 assert(pw_d3d9_private_set(&store,&key,source,1,0)==D3DERR_INVALIDCALL);
 struct pw_d3d9_private_data final={0};assert(SUCCEEDED(pw_d3d9_private_set(&final,&key,&object,sizeof(void *),D3DSPD_IUNKNOWN))&&references==2);
 pw_d3d9_private_dispose(&final);assert(references==1);
#ifdef _WIN64
 if(argc==2)actual_backend(argv[1],&object);
#else
 (void)argc;(void)argv;
#endif
 puts("PW_PRIVATE_DATA PASS copied=1 local_unknown=1 reentrant=1 bounded=1 cleanup=1");return 0;
}
