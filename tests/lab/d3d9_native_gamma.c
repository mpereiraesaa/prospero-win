/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <assert.h>
#include <stdio.h>
#include "../../wine/ps5/d3d9/pw_d3d9_native_gamma.h"
static void check(IDirect3DDevice9 *device)
{
 struct pw_d3d9_gamma_request q={.method=PW_D3D9_GAMMA_GET,.has_ramp=1},decoded;struct pw_d3d9_gamma_reply r,result;D3DGAMMARAMP original,direct;unsigned char wire[PW_D3D9_GAMMA_REQUEST_BYTES];
 IDirect3DDevice9_GetGammaRamp(device,0,&original);
 memset(q.ramp,0xa5,sizeof(q.ramp));assert(!pw_d3d9_gamma_request_encode(wire,sizeof(wire),&q));assert(!pw_d3d9_gamma_request_decode(&decoded,wire,sizeof(wire)));pw_d3d9_native_gamma_call(device,&decoded,&r);
 assert(!pw_d3d9_gamma_reply_encode(wire,PW_D3D9_GAMMA_REPLY_BYTES,&q,&r));assert(!pw_d3d9_gamma_reply_decode(&result,&q,wire,PW_D3D9_GAMMA_REPLY_BYTES));assert(result.hresult==S_OK&&!memcmp(result.ramp,&original,sizeof(original)));
 q.method=PW_D3D9_GAMMA_SET;q.flags=0xfedcba98u;for(unsigned i=0;i<256;i++){q.ramp[i]=(WORD)(i*257);q.ramp[256+i]=(WORD)(i*i*65535u/65025u);q.ramp[512+i]=(WORD)(i*257);}
 pw_d3d9_native_gamma_call(device,&q,&r);assert(r.hresult==S_OK&&!r.has_ramp);IDirect3DDevice9_GetGammaRamp(device,0,&direct);assert(!memcmp(&direct,q.ramp,sizeof(direct)));
 q.method=PW_D3D9_GAMMA_GET;q.flags=0;q.swapchain=0xffffffffu;memset(q.ramp,0xa5,sizeof(q.ramp));pw_d3d9_native_gamma_call(device,&q,&r);assert(r.hresult==S_OK&&!memcmp(q.ramp,r.ramp,sizeof(q.ramp)));
 q.method=PW_D3D9_GAMMA_SET;pw_d3d9_native_gamma_call(device,&q,&r);assert(r.hresult==S_OK);IDirect3DDevice9_GetGammaRamp(device,0,&direct);assert(memcmp(&direct,q.ramp,sizeof(direct)));
 q.swapchain=0;q.has_ramp=0;memset(q.ramp,0,sizeof(q.ramp));pw_d3d9_native_gamma_call(device,&q,&r);assert(r.hresult==S_OK);q.method=PW_D3D9_GAMMA_GET;pw_d3d9_native_gamma_call(device,&q,&r);assert(r.hresult==S_OK&&!r.has_ramp);
 IDirect3DDevice9_SetGammaRamp(device,0,0,&original);puts("PW_NATIVE_GAMMA PASS");
}
int wmain(int argc,WCHAR **argv)
{
 HMODULE module;IDirect3D9 *(WINAPI *factory)(UINT);IDirect3D9 *d3d;IDirect3DDevice9 *device=NULL;HWND window;D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};
 if(argc!=2)return 2;
 cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW_NATIVE_GAMMA";assert(RegisterClassW(&cls));
 window=CreateWindowW(cls.lpszClassName,L"query",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);assert(window);
 module=LoadLibraryW(argv[1]);assert(module);factory=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);d3d=factory(D3D_SDK_VERSION);assert(d3d);
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.hDeviceWindow=window;
 assert(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device)==S_OK&&device);
 check(device);
 IDirect3DDevice9_Release(device);IDirect3D9_Release(d3d);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);FreeLibrary(module);puts("PW_NATIVE_GAMMA PASS");return 0;
}
