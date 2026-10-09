/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN64
#include "pw_d3d9_native_up.h"
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
 WCHAR path[260];HMODULE backend=NULL;IDirect3D9 *d3d=NULL;IDirect3DDevice9 *device=NULL,*parent=NULL;
 IDirect3D9 *(WINAPI *factory)(UINT);D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};HWND window=NULL;
 IDirect3DSurface9 *target=NULL,*readback=NULL;IDirect3DVertexBuffer9 *vb=NULL,*bound=NULL;IDirect3DIndexBuffer9 *ib=NULL,*bound_ib=NULL;
 struct vertex {float x,y,z,w;DWORD color;} v[5]={{0}};WORD indices[3]={2,3,4};unsigned char storage[sizeof(v)+sizeof(indices)];
 struct pw_d3d9_up_upload u={0};D3DLOCKED_RECT locked;UINT offset=0,stride=0;DWORD error=1,pixel;HRESULT hr,direct;unsigned i;
 if(*(volatile LONG *)((char *)NtCurrentTeb()+0x180c))return 2;
 cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW-NativeUP";
 if(!RegisterClassW(&cls))goto done;
 window=CreateWindowExW(0,cls.lpszClassName,L"up proof",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);if(!window)goto done;
 if(!GetEnvironmentVariableW(L"PW_UP_BACKEND",path,260) || !(backend=LoadLibraryW(path)))goto done;
 factory=(void *)GetProcAddress(backend,"Direct3DCreate9");if(!factory || !(d3d=factory(D3D_SDK_VERSION)))goto done;
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.BackBufferWidth=pp.BackBufferHeight=64;pp.hDeviceWindow=window;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);result[0]=(uint32_t)hr;if(FAILED(hr))goto done;
#define CHECK(expr) do { hr=(expr);if(FAILED(hr)){result[3]=(uint32_t)hr;goto done;} }while(0)
 u.storage=storage;u.capacity=sizeof(storage);u.transfer=u.ready=1;u.draw=(struct pw_d3d9_up_draw){83,4,0,0,0,20,0,0,0};
 direct=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,0,v,20);
 hr=pw_d3d9_native_up_dispatch(device,&u);if(hr!=direct)goto done;result[1]++;
 CHECK(IDirect3DDevice9_SetFVF(device,D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
 direct=IDirect3DDevice9_DrawPrimitiveUP(device,D3DPT_TRIANGLELIST,0,v,20);
 hr=pw_d3d9_native_up_dispatch(device,&u);if(hr!=direct)goto done;result[1]++;
 CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE));CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_CULLMODE,D3DCULL_NONE));
 CHECK(IDirect3DDevice9_CreateVertexBuffer(device,256,0,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,D3DPOOL_DEFAULT,&vb,NULL));
 CHECK(IDirect3DDevice9_CreateIndexBuffer(device,32,0,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&ib,NULL));
 CHECK(IDirect3DDevice9_GetRenderTarget(device,0,&target));CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device,64,64,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL));
 for(i=0;i<2;i++){
  unsigned j;DWORD color=i?0xff00ff00:0xffff0000;
  v[2]=(struct vertex){0,0,0,1,color};v[3]=(struct vertex){63,0,0,1,color};v[4]=(struct vertex){0,63,0,1,color};
  CHECK(IDirect3DDevice9_SetStreamSource(device,0,vb,0,20));CHECK(IDirect3DDevice9_SetIndices(device,ib));
  CHECK(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0));CHECK(IDirect3DDevice9_BeginScene(device));
  u.draw=(struct pw_d3d9_up_draw){i?84:83,4,i?2:0,i?3:0,1,20,i?101:0,0,0};
  if(pw_d3d9_up_measure(&u.draw))goto done;
  memcpy(storage,i?(void *)v:(void *)(v+2),u.draw.vertex_bytes);
  if(i)memcpy(storage+u.draw.vertex_bytes,indices,sizeof(indices));
  u.total=u.received=u.draw.vertex_bytes+u.draw.index_bytes;
  CHECK(pw_d3d9_native_up_dispatch(device,&u));CHECK(IDirect3DDevice9_EndScene(device));
  CHECK(IDirect3DDevice9_GetStreamSource(device,0,&bound,&offset,&stride));if(bound||offset||stride)goto done;
  CHECK(IDirect3DDevice9_GetIndices(device,&bound_ib));if(i&&bound_ib)goto done;if(bound_ib){IDirect3DIndexBuffer9_Release(bound_ib);bound_ib=NULL;}
  CHECK(IDirect3DDevice9_GetRenderTargetData(device,target,readback));CHECK(IDirect3DSurface9_LockRect(readback,&locked,NULL,D3DLOCK_READONLY));
  memcpy(&pixel,(unsigned char *)locked.pBits+16*locked.Pitch+16*4,4);CHECK(IDirect3DSurface9_UnlockRect(readback));
  if((pixel&0xffffff)!=(color&0xffffff))goto done;
  result[1]++;
  /* Retain exact payload prefix; the backend may not mutate owned bytes. */
  for(j=0;j<u.draw.vertex_bytes;j++)if(storage[j]!=((unsigned char *)(i?(void *)v:(void *)(v+2)))[j])goto done;
 }
 error=0;
 done:
 if(parent)IDirect3DDevice9_Release(parent);
 if(bound)IDirect3DVertexBuffer9_Release(bound);
 if(bound_ib)IDirect3DIndexBuffer9_Release(bound_ib);
 if(vb)IDirect3DVertexBuffer9_Release(vb);
 if(ib)IDirect3DIndexBuffer9_Release(ib);
 if(target)IDirect3DSurface9_Release(target);
 if(readback)IDirect3DSurface9_Release(readback);
 if(device)IDirect3DDevice9_Release(device);
 if(d3d)IDirect3D9_Release(d3d);
 if(window)DestroyWindow(window);
 if(cls.lpszClassName)UnregisterClassW(cls.lpszClassName,cls.hInstance);
 if(backend)FreeLibrary(backend);
 result[7]=error;return error;
}
#else
struct request {ULONG version,size;WCHAR path[260];uint64_t result[8];};
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
int main(int argc,char **argv)
{
 struct request r={0};ULONG returned=0;LONG status;query_fn query;
 if(argc!=2)return 1;
 r.version=1;r.size=sizeof(r);MultiByteToWideChar(CP_UTF8,0,argv[1],-1,r.path,260);
 query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");if(!query)return 2;
 status=query(GetCurrentProcess(),0x50570001,&r,sizeof(r),&returned);
 printf("PW_UP_PE status=%08lx create=%08llx checks=%llu error=%llu\n",status,r.result[0],r.result[1],r.result[7]);
 return status || returned!=sizeof(r) || r.result[0] || r.result[1]!=4 || r.result[7];
}
#endif
