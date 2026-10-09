/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN64
#include "pw_d3d9_cursor.h"
static HRESULT acquire(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *device,void **out)
{IDirect3DSurface9 *surface=context;(void)device;if(id!=1||generation!=1||kind!=6)return D3DERR_INVALIDCALL;IDirect3DSurface9_AddRef(surface);*out=surface;return S_OK;}
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
 WCHAR path[260];HMODULE backend=NULL;IDirect3D9 *d3d=NULL;IDirect3DDevice9 *device=NULL,*parent=NULL;
 IDirect3D9 *(WINAPI *factory)(UINT);D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};HWND window=NULL;
 IDirect3DSurface9 *surface=NULL;struct pw_d3d9_cursor_request q;struct pw_d3d9_cursor_reply reply;DWORD error=1;HRESULT hr,direct;BOOL shown;D3DLOCKED_RECT locked;POINT old_position;GetCursorPos(&old_position);
 if(*(volatile LONG *)((char *)NtCurrentTeb()+0x180c))return 2;
 cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW-NativeCursor";
 if(!RegisterClassW(&cls))goto done;
 window=CreateWindowExW(0,cls.lpszClassName,L"cursor proof",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);if(!window)goto done;
 if(!GetEnvironmentVariableW(L"PW_CURSOR_BACKEND",path,260) || !(backend=LoadLibraryW(path)))goto done;
 factory=(void *)GetProcAddress(backend,"Direct3DCreate9");if(!factory || !(d3d=factory(D3D_SDK_VERSION)))goto done;
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=pp.BackBufferHeight=64;pp.hDeviceWindow=window;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);result[0]=(uint32_t)hr;if(FAILED(hr))goto done;
#define CHECK(expr) if(FAILED(expr))goto done
 CHECK(IDirect3DDevice9_CreateOffscreenPlainSurface(device,32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&surface,NULL));
 CHECK(IDirect3DSurface9_LockRect(surface,&locked,NULL,0));for(unsigned y=0;y<32;y++)memset((char *)locked.pBits+y*locked.Pitch,0xff,32*4);CHECK(IDirect3DSurface9_UnlockRect(surface));
 direct=IDirect3DDevice9_SetCursorProperties(device,7,9,surface);q=(struct pw_d3d9_cursor_request){10,7,9,0,1,1};hr=pw_d3d9_native_cursor(device,&q,&reply,acquire,surface);if(hr!=direct||reply.hresult!=(uint32_t)direct)goto done;result[1]++;
 direct=IDirect3DDevice9_SetCursorProperties(device,0,0,NULL);q=(struct pw_d3d9_cursor_request){10,0,0,0,0,0};hr=pw_d3d9_native_cursor(device,&q,&reply,acquire,surface);if(hr!=direct)goto done;result[1]++;
 q=(struct pw_d3d9_cursor_request){11,(uint32_t)-17,(uint32_t)-9,D3DCURSOR_IMMEDIATE_UPDATE,0,0};CHECK(pw_d3d9_native_cursor(device,&q,&reply,acquire,surface));if(reply.value||reply.hresult)goto done;result[1]++;
 IDirect3DDevice9_ShowCursor(device,FALSE);shown=IDirect3DDevice9_ShowCursor(device,(BOOL)0xffffffff);IDirect3DDevice9_ShowCursor(device,FALSE);
 q=(struct pw_d3d9_cursor_request){12,0xffffffff,0,0,0,0};CHECK(pw_d3d9_native_cursor(device,&q,&reply,acquire,surface));if(reply.value!=(uint32_t)shown)goto done;result[1]++;
 IDirect3DDevice9_ShowCursor(device,FALSE);
 error=0;
 done:
 if(parent)IDirect3DDevice9_Release(parent);
 if(surface)IDirect3DSurface9_Release(surface);
 SetCursorPos(old_position.x,old_position.y);
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
 printf("PW_CURSOR_PE status=%08lx create=%08llx checks=%llu error=%llu\n",status,r.result[0],r.result[1],r.result[7]);
 return status || returned!=sizeof(r) || r.result[0] || r.result[1]!=4 || r.result[7];
}
#endif
