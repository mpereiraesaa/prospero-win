/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
int wmain(int argc,WCHAR **argv)
{
 if(argc!=2)return 2;
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);
 IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);
 for(unsigned i=0;i<3;i++){
  struct pw_d3d9_device_request q={.operation=PW_D3D9_DEVICE_CREATE,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING};struct pw_d3d9_device_reply r;struct pw_d3d9_native_device *device=NULL;
  pw_d3d9_native_device_call(d3d,NULL,&q,&r,&device);assert(r.hresult==S_OK&&device);
  IDirect3DDevice9 *backend=pw_d3d9_native_device_backend(device);
 IDirect3DTexture9 *texture=NULL;IDirect3DSurface9 *surface=NULL,*old=NULL;D3DSURFACE_DESC desc;
 assert(IDirect3DDevice9_GetDepthStencilSurface(backend,&old)==S_OK&&old);
 assert(IDirect3DDevice9_CreateTexture(backend,64,64,1,D3DUSAGE_DEPTHSTENCIL,(D3DFORMAT)MAKEFOURCC('I','N','T','Z'),D3DPOOL_DEFAULT,&texture,NULL)==S_OK&&texture);
 assert(IDirect3DTexture9_GetSurfaceLevel(texture,0,&surface)==S_OK&&surface);assert(IDirect3DSurface9_GetDesc(surface,&desc)==S_OK&&desc.Format==(D3DFORMAT)MAKEFOURCC('I','N','T','Z'));
 assert(IDirect3DDevice9_SetDepthStencilSurface(backend,surface)==S_OK);assert(IDirect3DDevice9_Clear(backend,0,NULL,D3DCLEAR_ZBUFFER,0,0.5f,0)==S_OK);
 assert(IDirect3DDevice9_SetDepthStencilSurface(backend,old)==S_OK);IDirect3DSurface9_Release(old);IDirect3DSurface9_Release(surface);IDirect3DTexture9_Release(texture);
  puts("PW_INTZ_CONTROL status=0");fflush(stdout);
  assert(pw_d3d9_native_device_destroy(device));assert(pw_d3d9_native_device_shutdown());
 }
 IDirect3D9_Release(d3d);FreeLibrary(module);return 0;
}
