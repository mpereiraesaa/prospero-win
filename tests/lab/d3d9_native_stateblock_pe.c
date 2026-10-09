/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN64
#include "pw_d3d9_native_stateblock.h"
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
 WCHAR path[260];HMODULE backend=NULL;IDirect3D9 *d3d=NULL;IDirect3DDevice9 *device=NULL,*parent=NULL;
 IDirect3D9 *(WINAPI *factory)(UINT);D3DPRESENT_PARAMETERS pp={0};WNDCLASSW cls={0};HWND window=NULL;
 IDirect3DStateBlock9 *block=NULL;struct pw_d3d9_stateblock_request q;DWORD error=1,value=0;HRESULT hr;
 if(*(volatile LONG *)((char *)NtCurrentTeb()+0x180c))return 2;
 cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW-NativeStateblock";
 if(!RegisterClassW(&cls))goto done;
 window=CreateWindowExW(0,cls.lpszClassName,L"stateblock proof",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);if(!window)goto done;
 if(!GetEnvironmentVariableW(L"PW_STATEBLOCK_BACKEND",path,260) || !(backend=LoadLibraryW(path)))goto done;
 factory=(void *)GetProcAddress(backend,"Direct3DCreate9");if(!factory || !(d3d=factory(D3D_SDK_VERSION)))goto done;
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=pp.BackBufferHeight=64;pp.hDeviceWindow=window;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);result[0]=(uint32_t)hr;if(FAILED(hr))goto done;
#define CALL(method,type) (q=(struct pw_d3d9_stateblock_request){method,type},pw_d3d9_native_stateblock_dispatch(device,block,&q,&block))
#define CHECK(expr) if(FAILED(expr))goto done
 CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));CHECK(CALL(59,D3DSBT_ALL));
 CHECK(IDirect3DStateBlock9_GetDevice(block,&parent));if(parent!=device)goto done;IDirect3DDevice9_Release(parent);parent=NULL;
 CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,TRUE));CHECK(CALL(5,0));
 CHECK(IDirect3DDevice9_GetRenderState(device,D3DRS_ZENABLE,&value));if(value!=FALSE)goto done;result[1]++;
 CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,TRUE));CHECK(CALL(4,0));
 CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));CHECK(CALL(5,0));
 CHECK(IDirect3DDevice9_GetRenderState(device,D3DRS_ZENABLE,&value));if(value!=TRUE)goto done;result[1]++;
 IDirect3DStateBlock9_Release(block);block=NULL;
 CHECK(CALL(60,0));CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,FALSE));CHECK(CALL(61,0));
 CHECK(CALL(4,0));CHECK(IDirect3DDevice9_GetRenderState(device,D3DRS_ZENABLE,&value));result[2]=value;
 CHECK(IDirect3DDevice9_SetRenderState(device,D3DRS_ZENABLE,!value));CHECK(CALL(5,0));
 CHECK(IDirect3DDevice9_GetRenderState(device,D3DRS_ZENABLE,&value));if(value!=result[2])goto done;result[1]++;
 error=0;
 done:
 if(parent)IDirect3DDevice9_Release(parent);
 if(block)IDirect3DStateBlock9_Release(block);
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
 printf("PW_STATEBLOCK_PE status=%08lx create=%08llx restored=%llu error=%llu\n",status,r.result[0],r.result[1],r.result[7]);
 return status || returned!=sizeof(r) || r.result[0] || r.result[1]!=3 || r.result[7];
}
#endif
