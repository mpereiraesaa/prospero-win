/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* TEST ONLY: ordinary native window replaces PS5 driver association. The real
 * session, registry, object/command codecs/helpers and DXVK calls remain intact. */
#define COBJMACROS
#include "pw_d3d9_session.h"
#include <d3d9.h>
#include <stdio.h>
struct pw_d3d9_native_device {IDirect3DDevice9 *device;HWND window;unsigned implicit_phase;int reset_preserved,reset_succeeded,mirror_failed;};
unsigned pw_d3d9_native_device_implicit_phase(struct pw_d3d9_native_device *d){return d->implicit_phase;}
void pw_d3d9_native_device_implicit_set_phase(struct pw_d3d9_native_device *d,unsigned p){d->implicit_phase=p;}
int pw_d3d9_native_device_reset_preserved(struct pw_d3d9_native_device *d){return d->reset_preserved;}
int pw_d3d9_native_device_reset_succeeded(struct pw_d3d9_native_device *d){return d->reset_succeeded;}
static ATOM atom;static const WCHAR class_name[]=L"PW-SurfaceLifetime";
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *d){return d?d->device:NULL;}
uintptr_t pw_d3d9_native_device_identity(struct pw_d3d9_native_device *d){IUnknown *id=NULL;HRESULT hr=IDirect3DDevice9_QueryInterface(d->device,&IID_IUnknown,(void **)&id);if(FAILED(hr))return 0;uintptr_t result=(uintptr_t)id;IUnknown_Release(id);return result;}
int pw_d3d9_native_device_destroy(struct pw_d3d9_native_device *d){if(d->device)IDirect3DDevice9_Release(d->device);if(d->window)DestroyWindow(d->window);HeapFree(GetProcessHeap(),0,d);return 1;}
int pw_d3d9_native_device_shutdown(void){if(atom){UnregisterClassW(class_name,GetModuleHandleW(NULL));atom=0;}return 1;}
void pw_d3d9_native_device_call(void *factory,struct pw_d3d9_native_device *device,const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r,struct pw_d3d9_native_device **created)
{
 *r=(struct pw_d3d9_device_reply){.operation=q->operation,.hresult=D3DERR_INVALIDCALL,.parameters=q->parameters};*created=NULL;
 if(q->operation==PW_D3D9_DEVICE_RESET){
  device->reset_preserved=1;device->reset_succeeded=0;D3DPRESENT_PARAMETERS p={0};
#define COPY(name,native) p.native=q->parameters.name;
  PW_D3D9_PRESENT_FIELDS(COPY)
#undef COPY
  p.hDeviceWindow=device->window;
  /* Fixture-only marker never reaches DXVK. Simulate post-native mirror failure
   * once while retaining the native Reset outcome for shipping orchestration. */
  int mirror=(p.Flags&0x80000000u)&&!device->mirror_failed;p.Flags&=~0x80000000u;
  HRESULT native=IDirect3DDevice9_Reset(device->device,&p);
  device->reset_succeeded=SUCCEEDED(native);
  device->reset_preserved=FAILED(native)&&IDirect3DDevice9_TestCooperativeLevel(device->device)==S_OK;
  #define COPY_BACK(name,native) r->parameters.name=p.native;
  PW_D3D9_PRESENT_FIELDS(COPY_BACK)
#undef COPY_BACK
  r->hresult=native;
  if(SUCCEEDED(native)&&mirror){device->mirror_failed=1;r->hresult=D3DERR_DEVICELOST;fprintf(stderr,"SURFACE_TEST_MIRROR native=00000000 final=88760868 preserved=0\n");}
  return;
 }
 if(q->operation!=PW_D3D9_DEVICE_CREATE)return;
 if(!atom){WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=class_name};atom=RegisterClassW(&cls);if(!atom){r->hresult=E_FAIL;return;}}
 struct pw_d3d9_native_device *d=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*d));if(!d){r->hresult=E_OUTOFMEMORY;return;}
 d->window=CreateWindowExW(0,class_name,L"surface lifetime",WS_POPUP,0,0,64,64,NULL,NULL,GetModuleHandleW(NULL),NULL);
 if(!d->window){pw_d3d9_native_device_destroy(d);r->hresult=E_FAIL;return;}
 D3DPRESENT_PARAMETERS p={0};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.BackBufferWidth=p.BackBufferHeight=64;p.BackBufferFormat=D3DFMT_A8R8G8B8;p.hDeviceWindow=d->window;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;p.EnableAutoDepthStencil=TRUE;p.AutoDepthStencilFormat=D3DFMT_D16;
 r->hresult=IDirect3D9_CreateDevice((IDirect3D9 *)factory,q->adapter,q->device_type,d->window,q->behavior_flags,&p,&d->device);
 if(FAILED((HRESULT)r->hresult)){pw_d3d9_native_device_destroy(d);return;}
#define COPY_BACK(name,native) r->parameters.name=p.native;
 PW_D3D9_PRESENT_FIELDS(COPY_BACK)
#undef COPY_BACK
 *created=d;
}
