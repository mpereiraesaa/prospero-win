/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include <d3d9.h>
#include <assert.h>
int wmain(int argc,WCHAR **argv)
{
 if(argc!=2)return 2;
 HMODULE module=LoadLibraryW(argv[1]);assert(module);IDirect3D9 *(WINAPI *factory)(UINT)=(void *)GetProcAddress(module,"Direct3DCreate9");assert(factory);
 IDirect3D9 *d3d=factory(D3D_SDK_VERSION);assert(d3d);
 for(unsigned i=0;i<3;i++){
  struct pw_d3d9_device_request q={.operation=PW_D3D9_DEVICE_CREATE,.device_type=D3DDEVTYPE_HAL,.behavior_flags=D3DCREATE_HARDWARE_VERTEXPROCESSING};struct pw_d3d9_device_reply r;struct pw_d3d9_native_device *device=NULL;
  pw_d3d9_native_device_call(d3d,NULL,&q,&r,&device);assert(r.hresult==S_OK&&device);assert(pw_d3d9_native_device_destroy(device));assert(pw_d3d9_native_device_shutdown());
 }
 IDirect3D9_Release(d3d);FreeLibrary(module);return 0;
}
