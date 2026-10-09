/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#define CHECK(x) do{HRESULT h=(x);if(FAILED(h)){fprintf(stderr,"line=%u hr=%08lx\n",__LINE__,(DWORD)h);return 10;}}while(0)
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"line=%u assertion\n",__LINE__);return 11;}}while(0)
static int reset_cases(IDirect3DDevice9 *d,D3DPRESENT_PARAMETERS *p,int bridge,unsigned cycle)
{
 IDirect3DSurface9 *rt=NULL,*ds=NULL,*again=NULL,*blocker=NULL;D3DSURFACE_DESC desc;
 CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&rt));CHECK(IDirect3DDevice9_GetDepthStencilSurface(d,&ds));
 IDirect3DSurface9_Release(rt);IDirect3DSurface9_Release(ds);
 CHECK(IDirect3DDevice9_SetDepthStencilSurface(d,NULL));
 D3DPRESENT_PARAMETERS bad=*p;bad.SwapEffect=0;
 HRESULT early=IDirect3DDevice9_Reset(d,&bad);REQUIRE(early==D3DERR_INVALIDCALL);
 CHECK(IDirect3DDevice9_TestCooperativeLevel(d));
 CHECK(IDirect3DDevice9_SetRenderTarget(d,0,rt));CHECK(IDirect3DDevice9_SetDepthStencilSurface(d,ds));
 CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&again));REQUIRE(again==rt);IDirect3DSurface9_Release(again);
 CHECK(IDirect3DDevice9_GetDepthStencilSurface(d,&again));REQUIRE(again==ds);IDirect3DSurface9_Release(again);
 /* An explicit live DEFAULT resource forces the late NotReset failure after
  * DXVK clears implicit ownership. Old zero-public addresses are not reused. */
 CHECK(IDirect3DDevice9_CreateRenderTarget(d,16,16,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&blocker,NULL));
 HRESULT late=IDirect3DDevice9_Reset(d,p);REQUIRE(late==D3DERR_INVALIDCALL);
 REQUIRE(IDirect3DDevice9_TestCooperativeLevel(d)==D3DERR_DEVICENOTRESET);
 again=NULL;REQUIRE(IDirect3DDevice9_GetRenderTarget(d,0,&again)==D3DERR_NOTFOUND);REQUIRE(!again);
 again=NULL;REQUIRE(IDirect3DDevice9_GetDepthStencilSurface(d,&again)==D3DERR_NOTFOUND);REQUIRE(!again);
 CHECK(IDirect3DSurface9_GetDesc(blocker,&desc));IDirect3DSurface9_Release(blocker);CHECK(IDirect3DDevice9_Reset(d,p));
 /* Publicly held implicit surfaces survive the late failure and cause Reset
  * rejection themselves. Releasing them permits a subsequent successful Reset. */
 CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&rt));CHECK(IDirect3DDevice9_GetDepthStencilSurface(d,&ds));
 HRESULT owned=IDirect3DDevice9_Reset(d,p);REQUIRE(owned==D3DERR_INVALIDCALL);
 REQUIRE(IDirect3DDevice9_TestCooperativeLevel(d)==D3DERR_DEVICENOTRESET);
 CHECK(IDirect3DSurface9_GetDesc(rt,&desc));CHECK(IDirect3DSurface9_GetDesc(ds,&desc));
 IDirect3DSurface9_Release(rt);IDirect3DSurface9_Release(ds);CHECK(IDirect3DDevice9_Reset(d,p));
 printf("SURFACE_RESET bridge=%d cycle=%u early=%08lx late=%08lx owned=%08lx recovered=1\n",bridge,cycle,(DWORD)early,(DWORD)late,(DWORD)owned);fflush(stdout);return 0;
}
int wmain(int argc,WCHAR **argv){
 REQUIRE(argc==2||argc==4||argc==5);int bridge=argc>=4;
 int expected_failure=argc==5;REQUIRE(!expected_failure||!wcscmp(argv[4],L"--expect-proxy-failure"));
 if(bridge){SetEnvironmentVariableW(L"PW_D3D9_SERVICE64",argv[2]);SetEnvironmentVariableW(L"PW_D3D9_BACKEND64",argv[3]);}
 WNDCLASSW c={.lpfnWndProc=DefWindowProcW,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"SurfaceLifetime"};REQUIRE(RegisterClassW(&c));
 HWND w=CreateWindowW(c.lpszClassName,L"surface lifetime",WS_POPUP,0,0,64,64,NULL,NULL,c.hInstance,NULL);REQUIRE(w);
 HMODULE m=LoadLibraryW(argv[1]);REQUIRE(m);IDirect3D9 *(WINAPI *create)(UINT)=(void*)GetProcAddress(m,"Direct3DCreate9");REQUIRE(create);
 for(unsigned cycle=0;cycle<3;cycle++){
 IDirect3D9 *f=create(D3D_SDK_VERSION);REQUIRE(f);IDirect3DDevice9 *d=NULL;
 D3DPRESENT_PARAMETERS p={.Windowed=TRUE,.SwapEffect=D3DSWAPEFFECT_DISCARD,.BackBufferFormat=D3DFMT_A8R8G8B8,.BackBufferWidth=64,.BackBufferHeight=64,.hDeviceWindow=w,.EnableAutoDepthStencil=TRUE,.AutoDepthStencilFormat=D3DFMT_D16};
 CHECK(IDirect3D9_CreateDevice(f,0,D3DDEVTYPE_HAL,w,D3DCREATE_HARDWARE_VERTEXPROCESSING,&p,&d));
 for(unsigned reset=0;reset<2;reset++){
 IDirect3DSurface9 *rt=NULL,*ds=NULL,*again_rt=NULL,*again_ds=NULL;
 CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&rt));CHECK(IDirect3DDevice9_GetDepthStencilSurface(d,&ds));
 ULONG rt_refs=IDirect3DSurface9_Release(rt),ds_refs=IDirect3DSurface9_Release(ds);
 /* Exact installed RenderWare pattern: retain the default surface address
  * after releasing the getter's public reference, then pass it to the device.
  * The fixture never directly dereferences that released address. */
 HRESULT rh=IDirect3DDevice9_SetRenderTarget(d,0,rt),dh=IDirect3DDevice9_SetDepthStencilSurface(d,ds);
 CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&again_rt));CHECK(IDirect3DDevice9_GetDepthStencilSurface(d,&again_ds));
 printf("SURFACE_LIFETIME bridge=%d cycle=%u reset=%u rt_release=%lu ds_release=%lu rt_set=%08lx ds_set=%08lx rt_identity=%u ds_identity=%u\n",bridge,cycle,reset,rt_refs,ds_refs,(DWORD)rh,(DWORD)dh,rt==again_rt,ds==again_ds);fflush(stdout);
 REQUIRE(rh==(expected_failure?D3DERR_INVALIDCALL:S_OK));REQUIRE(dh==(expected_failure?D3DERR_INVALIDCALL:S_OK));
 /* Reacquiring owned references restores normal valid bridge binding. */
 CHECK(IDirect3DDevice9_SetRenderTarget(d,0,again_rt));CHECK(IDirect3DDevice9_SetDepthStencilSurface(d,again_ds));
 IDirect3DSurface9_Release(again_rt);IDirect3DSurface9_Release(again_ds);
 if(!reset){CHECK(IDirect3DDevice9_Reset(d,&p));/* Never use pre-Reset addresses afterward. */}
 }
 if(!expected_failure){int result=reset_cases(d,&p,bridge,cycle);if(result)return result;
  if(bridge){
   IDirect3DSurface9 *old=NULL;CHECK(IDirect3DDevice9_GetRenderTarget(d,0,&old));IDirect3DSurface9_Release(old);
   p.Flags|=0x80000000u;REQUIRE(IDirect3DDevice9_Reset(d,&p)==D3DERR_DEVICELOST);p.Flags&=~0x80000000u;
   /* Address-only frontend resolver must reject this retired shell. This does
    * not call through/dereference the released surface's COM vtable. */
   REQUIRE(IDirect3DDevice9_SetRenderTarget(d,0,old)==D3DERR_INVALIDCALL);
   printf("SURFACE_MIRROR cycle=%u native_success=1 final_failed=1 retired=1\n",cycle);fflush(stdout);
  }
 }
 REQUIRE(IDirect3DDevice9_Release(d)==0);REQUIRE(IDirect3D9_Release(f)==0);
 }
 DestroyWindow(w);FreeLibrary(m);puts("SURFACE_CLOSE status=0");return 0;
}
