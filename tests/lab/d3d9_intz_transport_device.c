/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* TEST ONLY: ordinary native window replaces PS5 driver association. The real
 * session, registry, stateblock codecs/helpers and DXVK calls remain intact. */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_session.h"
#include <d3d9.h>
#include <assert.h>
#include <stdio.h>
struct pw_d3d9_native_device {IDirect3DDevice9 *device;HWND window;};
static ATOM atom;static const WCHAR class_name[]=L"PW-StateblockTransport";
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
void *pw_d3d9_native_device_backend(struct pw_d3d9_native_device *d){return d?d->device:NULL;}
uintptr_t pw_d3d9_native_device_identity(struct pw_d3d9_native_device *d){IUnknown *id=NULL;HRESULT hr=IDirect3DDevice9_QueryInterface(d->device,&IID_IUnknown,(void **)&id);if(FAILED(hr))return 0;uintptr_t result=(uintptr_t)id;IUnknown_Release(id);return result;}
int pw_d3d9_native_device_destroy(struct pw_d3d9_native_device *d){if(d->device)IDirect3DDevice9_Release(d->device);if(d->window)DestroyWindow(d->window);HeapFree(GetProcessHeap(),0,d);return 1;}
int pw_d3d9_native_device_shutdown(void){if(atom){UnregisterClassW(class_name,GetModuleHandleW(NULL));atom=0;}return 1;}
void pw_d3d9_native_device_call(void *factory,struct pw_d3d9_native_device *device,const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r,struct pw_d3d9_native_device **created)
{
 (void)device;*r=(struct pw_d3d9_device_reply){.operation=q->operation,.hresult=D3DERR_INVALIDCALL,.parameters=q->parameters};*created=NULL;
 if(q->operation!=PW_D3D9_DEVICE_CREATE)return;
 if(!atom){WNDCLASSW cls={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=class_name};atom=RegisterClassW(&cls);if(!atom){r->hresult=E_FAIL;return;}}
 struct pw_d3d9_native_device *d=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*d));if(!d){r->hresult=E_OUTOFMEMORY;return;}
 d->window=CreateWindowExW(0,class_name,L"stateblock transport",WS_POPUP,0,0,64,64,NULL,NULL,GetModuleHandleW(NULL),NULL);
 if(!d->window){pw_d3d9_native_device_destroy(d);r->hresult=E_FAIL;return;}
 D3DPRESENT_PARAMETERS p={0};p.Windowed=TRUE;p.SwapEffect=D3DSWAPEFFECT_DISCARD;p.BackBufferWidth=p.BackBufferHeight=64;p.BackBufferFormat=D3DFMT_A8R8G8B8;p.hDeviceWindow=d->window;p.EnableAutoDepthStencil=TRUE;p.AutoDepthStencilFormat=D3DFMT_D16;p.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 r->hresult=IDirect3D9_CreateDevice((IDirect3D9 *)factory,q->adapter,q->device_type,d->window,q->behavior_flags,&p,&d->device);
 if(FAILED((HRESULT)r->hresult)){pw_d3d9_native_device_destroy(d);return;}
 IDirect3DTexture9 *texture=NULL;IDirect3DSurface9 *surface=NULL,*old=NULL;D3DSURFACE_DESC desc;
 assert(IDirect3DDevice9_GetDepthStencilSurface(d->device,&old)==S_OK&&old);
 assert(IDirect3DDevice9_CreateTexture(d->device,64,64,1,D3DUSAGE_DEPTHSTENCIL,(D3DFORMAT)MAKEFOURCC('I','N','T','Z'),D3DPOOL_DEFAULT,&texture,NULL)==S_OK&&texture);
 assert(IDirect3DTexture9_GetSurfaceLevel(texture,0,&surface)==S_OK&&surface);assert(IDirect3DSurface9_GetDesc(surface,&desc)==S_OK&&desc.Format==(D3DFORMAT)MAKEFOURCC('I','N','T','Z'));
 assert(IDirect3DDevice9_SetDepthStencilSurface(d->device,surface)==S_OK);assert(IDirect3DDevice9_Clear(d->device,0,NULL,D3DCLEAR_ZBUFFER,0,0.5f,0)==S_OK);
 assert(IDirect3DDevice9_SetDepthStencilSurface(d->device,old)==S_OK);IDirect3DSurface9_Release(old);IDirect3DSurface9_Release(surface);IDirect3DTexture9_Release(texture);
 puts("PW_INTZ_CONTROL status=0");fflush(stdout);*created=d;
}
