/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Synthetic two-domain UI proof. No game proxy and no cross-domain WNDPROC. */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
#include "pw_d3d9_window_driver.h"
typedef ULONG_PTR (WINAPI *driver_call_fn)(ULONG_PTR,ULONG_PTR,ULONG);
static driver_call_fn driver_call;
#endif

#define NOTICE (WM_APP + 0x31)
struct window_state {
 uint32_t magic,epoch,window_id,generation;
 volatile LONG guest_calls,native_calls,guest_keys,error,guest_exceptions,native_exceptions,native_child_calls,guest_nulls,guest_builtins,native_builtins;
 uint32_t width,height,create_hr,reset_hr,present_hr;
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 volatile LONG driver_guest_ids,driver_attaches,driver_mirrors,driver_detaches,driver_rejects;
#endif
};
static struct window_state *state;
static BOOL fullscreen_probe;
#ifdef _WIN64
static uint32_t fullscreen_width=1920,fullscreen_height=1080;
#endif
static HANDLE acknowledge;
static WCHAR guest_name[96],service_name[96],mapping_name[96],event_name[96];
static void names(const WCHAR *session)
{
 swprintf(guest_name,96,L"PW-Guest32-%ls",session);
 swprintf(service_name,96,L"PW-Service64-%ls",session);
 swprintf(mapping_name,96,L"Local\\PW-Window-%ls",session);
 swprintf(event_name,96,L"Local\\PW-WindowAck-%ls",session);
}
/* Exercise Wine's builtin procedure handles without subclassing. A builtin
 * created before native user32 must remain callable afterwards. */
static int builtin_roundtrip(HWND window)
{
 WCHAR text[32];
 if(!window || !SetWindowTextW(window,L"domain-builtin"))return 0;
 if(GetWindowTextW(window,text,32)!=14 || lstrcmpW(text,L"domain-builtin"))return 0;
 SendMessageW(window,WM_NULL,0,0);return 1;
}
static HWND builtin_create(HWND parent)
{
 return CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,16,16,parent,NULL,GetModuleHandleW(NULL),NULL);
}
#ifdef _WIN64
static void checkpoint(const char *stage){fprintf(stderr,"PW_WINDOW_NATIVE stage=%s tid=%lu guest=%ld native=%ld\n",stage,GetCurrentThreadId(),state?state->guest_calls:0,state?state->native_calls:0);fflush(stderr);}
static int native_domain(void){return *(volatile LONG *)((char *)NtCurrentTeb()+0x180c)==0;}
static LONG CALLBACK native_exception(EXCEPTION_POINTERS *p)
{
 if(p->ExceptionRecord->ExceptionCode!=0xe0425750)return EXCEPTION_CONTINUE_SEARCH;
 if(!native_domain())InterlockedExchange(&state->error,91);
 InterlockedIncrement(&state->native_exceptions);return EXCEPTION_CONTINUE_EXECUTION;
}
static LRESULT CALLBACK service_proc(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
 if(state){if(!native_domain())InterlockedExchange(&state->error,90);InterlockedIncrement(&state->native_calls);}
 return DefWindowProcW(window,message,wp,lp);
}
static DWORD WINAPI native_window_child(void *unused)
{
 HWND window;(void)unused;if(!native_domain())return 1;
 window=CreateWindowExW(WS_EX_NOACTIVATE,service_name,L"native-child",WS_POPUP,0,0,16,16,NULL,NULL,NULL,NULL);
 if(!window)return 2;
 {HWND builtin=builtin_create(window);if(!builtin_roundtrip(builtin))return 3;
 DestroyWindow(builtin);InterlockedIncrement(&state->native_builtins);}
 SendMessageW(window,WM_NULL,0,0);InterlockedIncrement(&state->native_child_calls);
 RaiseException(0xe0425750,0,0,NULL);DestroyWindow(window);return 0;
}
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
static int driver_apply(HWND window,struct pw_d3d9_window_driver_request *q)
{
 BOOL applied;
 q->operation=PW_D3D9_WINDOW_QUERY_STATE;
 if(driver_call((ULONG_PTR)q,sizeof(*q),PW_D3D9_WINDOW_DRIVER_CALL))return 0;
 q->operation=PW_D3D9_WINDOW_BEGIN;q->sequence++;
 if(driver_call((ULONG_PTR)q,sizeof(*q),PW_D3D9_WINDOW_DRIVER_CALL))return 0;
 applied=SetWindowPos(window,NULL,q->state.x,q->state.y,q->state.width,q->state.height,
   SWP_NOACTIVATE|SWP_NOZORDER|((q->state.flags&PW_D3D9_WINDOW_VISIBLE)?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
 q->hresult=applied?0:0x80004005u;q->operation=PW_D3D9_WINDOW_ACK;
 if(driver_call((ULONG_PTR)q,sizeof(*q),PW_D3D9_WINDOW_DRIVER_CALL))return 0;
 if(q->state.width!=state->width || q->state.height!=state->height)return 0;
 InterlockedIncrement(&state->driver_mirrors);return 1;
}
#endif
static int notify_guest(HWND guest,unsigned phase)
{
 DWORD start=GetTickCount();MSG message;
 if(!PostMessageW(guest,NOTICE,phase,state->generation))return 0;
 while(GetTickCount()-start<5000){
  DWORD result=MsgWaitForMultipleObjects(1,&acknowledge,FALSE,100,QS_ALLINPUT);
  if(result==WAIT_OBJECT_0)return !state->error;
  if(result==WAIT_FAILED)return 0;
  while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
 }
 return 0;
}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
 WCHAR session[48],path[260],mode[4];HANDLE mapping=NULL,child=NULL;void *handler=NULL;DWORD child_code;BOOL device_test=TRUE;HWND guest=NULL,window=NULL;
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 struct pw_d3d9_window_driver_request association={0};BOOL attached=FALSE;
#endif
 WNDCLASSW cls={0};HMODULE backend=NULL;IDirect3D9 *d3d=NULL;IDirect3DDevice9 *device=NULL;
 IDirect3D9 *(WINAPI *factory)(UINT);LONG (WINAPI *register_callbacks)(void **);void *bogus_table[1]={NULL};D3DPRESENT_PARAMETERS pp={0};DWORD error=0;
 fullscreen_probe=GetEnvironmentVariableW(L"PW_BRIDGE_WINDOW_FULLSCREEN",mode,4)&&mode[0]==L'1';
 register_callbacks=(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"__wine_register_wow64_callbacks");
 if(!register_callbacks || register_callbacks(NULL)!=(LONG)0xc000000d || register_callbacks(bogus_table)!=(LONG)0xc000000d)return 31;
 if(!native_domain() || !GetEnvironmentVariableW(L"PW_BRIDGE_WINDOW_SESSION",session,48))return 10;
 device_test=!(GetEnvironmentVariableW(L"PW_BRIDGE_WINDOW_DEVICE",mode,4) && mode[0]==L'0');
 names(session);mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,mapping_name);
 if(!mapping)return 11;
 state=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*state));
 if(!state || state->magic!=0x57575042 || !state->epoch || !state->window_id || !state->generation){error=12;goto done;}
 acknowledge=OpenEventW(SYNCHRONIZE,FALSE,event_name);guest=FindWindowW(guest_name,guest_name);
 if(!acknowledge || !guest){error=13;goto done;}
 cls.lpfnWndProc=service_proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=service_name;
 if(!RegisterClassW(&cls)){error=14;goto done;}
 window=CreateWindowExW(WS_EX_NOACTIVATE,service_name,service_name,WS_POPUP,360,20,state->width,state->height,NULL,NULL,cls.hInstance,NULL);
 if(!window){error=15;goto done;}
 {HWND builtin=builtin_create(window);if(!builtin_roundtrip(builtin)){error=32;goto done;}
 DestroyWindow(builtin);InterlockedIncrement(&state->native_builtins);}
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 driver_call=(driver_call_fn)GetProcAddress(GetModuleHandleW(L"win32u.dll"),"NtUserCallTwoParam");
 if(!driver_call){error=33;goto done;}
 {struct pw_d3d9_guest_window_request invalid={.version=1,.size=sizeof(invalid),.operation=PW_D3D9_GUEST_REGISTER};
 if(driver_call((ULONG_PTR)window,(ULONG_PTR)&invalid,PW_D3D9_GUEST_WINDOW_CALL)!=PW_D3D9_WINDOW_INVALID){error=34;goto done;}
 InterlockedIncrement(&state->driver_rejects);}
 association.version=PW_D3D9_WINDOW_DRIVER_VERSION;association.size=sizeof(association);
 association.operation=PW_D3D9_WINDOW_ATTACH;
 association.guest=(struct pw_d3d9_window_id){state->epoch,state->window_id,state->generation+1};
 association.service=(uintptr_t)window;
 if(driver_call((ULONG_PTR)&association,sizeof(association),PW_D3D9_WINDOW_DRIVER_CALL)!=PW_D3D9_WINDOW_STALE){error=35;goto done;}
 InterlockedIncrement(&state->driver_rejects);association.guest.generation=state->generation;
 if(driver_call((ULONG_PTR)&association,sizeof(association),PW_D3D9_WINDOW_DRIVER_CALL)){error=36;goto done;}
 attached=TRUE;InterlockedIncrement(&state->driver_attaches);
 if(!driver_apply(window,&association)){error=37;goto done;}
#else
 ShowWindow(window,SW_SHOWNOACTIVATE);
#endif
 result[0]=(uintptr_t)NtCurrentTeb();result[1]=(uintptr_t)service_proc;
 handler=AddVectoredExceptionHandler(1,native_exception);if(!handler){error=27;goto done;}
 child=CreateThread(NULL,0,native_window_child,NULL,0,NULL);if(!child){error=28;goto done;}
 if(WaitForSingleObject(child,5000)!=WAIT_OBJECT_0)TerminateProcess(GetCurrentProcess(),29);
 GetExitCodeThread(child,&child_code);CloseHandle(child);child=NULL;if(child_code){error=30;goto done;}
 RaiseException(0xe0425750,0,0,NULL);
 if((uintptr_t)service_proc<=UINT32_MAX || !notify_guest(guest,1)){error=16;goto done;}
 if(device_test){
 if(!GetEnvironmentVariableW(L"PW_BRIDGE_BACKEND64",path,260) || !(backend=LoadLibraryW(path))){error=17;goto done;}
 factory=(void *)GetProcAddress(backend,"Direct3DCreate9");if(!factory || !(d3d=factory(D3D_SDK_VERSION))){error=18;goto done;}
 pp.Windowed=!fullscreen_probe;pp.SwapEffect=fullscreen_probe?D3DSWAPEFFECT_FLIP:D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=fullscreen_probe?D3DFMT_A8R8G8B8:D3DFMT_X8R8G8B8;
 if(fullscreen_probe){state->width=fullscreen_width;state->height=fullscreen_height;pp.EnableAutoDepthStencil=TRUE;pp.AutoDepthStencilFormat=D3DFMT_D16;}
 pp.BackBufferWidth=state->width;pp.BackBufferHeight=state->height;pp.hDeviceWindow=window;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 checkpoint("before-create-device");
 state->create_hr=IDirect3D9_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);
 checkpoint("after-create-device");
 result[4]=state->create_hr;if(FAILED((HRESULT)state->create_hr)){error=19;goto done;}
 }
 if(!notify_guest(guest,2)){error=20;goto done;}
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 if(!driver_apply(window,&association)){error=38;goto done;}
#else
 if(!fullscreen_probe&&!SetWindowPos(window,NULL,360,20,state->width,state->height,SWP_NOACTIVATE|SWP_NOZORDER)){error=21;goto done;}
#endif
 /* Explicit mirror delivered only to this native window's native procedure. */
 if(!fullscreen_probe){SendMessageW(window,WM_ACTIVATEAPP,FALSE,0);SendMessageW(window,WM_ACTIVATEAPP,TRUE,0);}
 if(device_test){
 pp.BackBufferWidth=fullscreen_probe?fullscreen_width:state->width;pp.BackBufferHeight=fullscreen_probe?fullscreen_height:state->height;
 checkpoint("before-reset");
 state->reset_hr=IDirect3DDevice9_Reset(device,&pp);result[5]=state->reset_hr;
 if(FAILED((HRESULT)state->reset_hr)){error=22;goto done;}
 if(FAILED(IDirect3DDevice9_Clear(device,0,NULL,D3DCLEAR_TARGET,0xff305080,1.0f,0))){error=23;goto done;}
 checkpoint("before-present");
 state->present_hr=IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL);result[6]=state->present_hr;
 if(FAILED((HRESULT)state->present_hr)){error=24;goto done;}
 }
 if(fullscreen_probe){
  RECT rect;GetWindowRect(window,&rect);
  fprintf(stderr,"PW_FULLSCREEN foreground=%p guest=%p service=%p guest_foreground=%u service_iconic=%u service_rect=%ld,%ld,%ld,%ld cooperative=%08lx\n",
    GetForegroundWindow(),guest,window,GetForegroundWindow()==guest,IsIconic(window),rect.left,rect.top,rect.right,rect.bottom,(unsigned long)IDirect3DDevice9_TestCooperativeLevel(device));
  if(GetForegroundWindow()!=guest||IsIconic(window)||(uint32_t)(rect.right-rect.left)!=fullscreen_width||(uint32_t)(rect.bottom-rect.top)!=fullscreen_height){error=41;goto done;}
 }
 if(!notify_guest(guest,3)){error=25;goto done;}
 done:
 checkpoint("cleanup");
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 if(attached){association.operation=PW_D3D9_WINDOW_CLOSE;
 if(driver_call((ULONG_PTR)&association,sizeof(association),PW_D3D9_WINDOW_DRIVER_CALL))error=39;}
#endif
 if(device)IDirect3DDevice9_Release(device);
 if(d3d)IDirect3D9_Release(d3d);
 if(window)DestroyWindow(window);
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 if(attached){association.operation=PW_D3D9_WINDOW_DETACH;
 if(driver_call((ULONG_PTR)&association,sizeof(association),PW_D3D9_WINDOW_DRIVER_CALL))error=40;
 else InterlockedIncrement(&state->driver_detaches);}
#endif
 if(cls.lpszClassName)UnregisterClassW(service_name,cls.hInstance);
 if(backend)FreeLibrary(backend);
 if(handler)RemoveVectoredExceptionHandler(handler);
 if(state){
  if(error)InterlockedExchange(&state->error,error);
  result[7]=state->native_calls;
  if(!error && !notify_guest(guest,4))error=26;
  UnmapViewOfFile(state);state=NULL;
 }
 if(acknowledge){CloseHandle(acknowledge);acknowledge=NULL;}
 CloseHandle(mapping);return error;
}
#else
struct request { ULONG version,size; WCHAR path[260]; uint64_t result[8]; };
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
static struct request request;
static LONG query_status;
static DWORD guest_thread;
static WNDPROC original_proc;
static HWND guest_builtin;
static LONG CALLBACK guest_exception(EXCEPTION_POINTERS *p)
{
 if(p->ExceptionRecord->ExceptionCode!=0xe0425751)return EXCEPTION_CONTINUE_SEARCH;
 if(!*(volatile LONG *)((char *)NtCurrentTeb()+0xfdc))InterlockedExchange(&state->error,82);
 InterlockedIncrement(&state->guest_exceptions);return EXCEPTION_CONTINUE_EXECUTION;
}
static LRESULT CALLBACK guest_proc(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
 if(state && (GetCurrentThreadId()!=guest_thread || !*(volatile LONG *)((char *)NtCurrentTeb()+0xfdc)))InterlockedExchange(&state->error,80);
 if(message==WM_NULL && state)InterlockedIncrement(&state->guest_nulls);
 if(message==WM_KEYDOWN && wp==VK_F9){InterlockedIncrement(&state->guest_keys);return 0;}
 if(message==NOTICE){
  if((uint32_t)lp!=state->generation || GetWindowLongPtrW(window,GWLP_WNDPROC)!=(LONG_PTR)original_proc)InterlockedExchange(&state->error,81);
  InterlockedIncrement(&state->guest_calls);
  if(wp==1){
   if(fullscreen_probe){SetForegroundWindow(window);SetFocus(window);}
   HWND during=builtin_create(window);
   if(!builtin_roundtrip(guest_builtin) || !builtin_roundtrip(during))InterlockedExchange(&state->error,83);
   if(during)DestroyWindow(during);
   InterlockedIncrement(&state->guest_builtins);PostMessageW(window,WM_KEYDOWN,VK_F9,1);
  }
  if(wp==2){if(fullscreen_probe){fprintf(stderr,"PW_FULLSCREEN_GUEST phase=2 foreground=%p guest=%p set=%u\n",GetForegroundWindow(),window,SetForegroundWindow(window));SetFocus(window);}
   RaiseException(0xe0425751,0,0,NULL);state->width=640;state->height=480;SetWindowPos(window,NULL,20,20,640,480,SWP_NOZORDER|SWP_NOACTIVATE);}
  if(wp==3)SetFocus(window);
  SetEvent(acknowledge);return 0;
 }
 return DefWindowProcW(window,message,wp,lp);
}
static DWORD WINAPI broker(void *unused)
{
 ULONG returned;query_fn query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");(void)unused;
 if(!query)return 1;
 query_status=query(GetCurrentProcess(),0x50570001,&request,sizeof(request),&returned);
 return query_status || returned!=sizeof(request);
}
int main(int argc,char **argv)
{
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 struct pw_d3d9_guest_window_request registration={0};
#endif
 WCHAR session[48];HANDLE mapping,thread;HWND window;WNDCLASSW cls={0};MSG message;DWORD code=1,start;void *handler;
 if(argc!=2)return 1;
 guest_thread=GetCurrentThreadId();
 {WCHAR mode[4];fullscreen_probe=GetEnvironmentVariableW(L"PW_BRIDGE_WINDOW_FULLSCREEN",mode,4)&&mode[0]==L'1';}
 if(!GetEnvironmentVariableW(L"PW_BRIDGE_WINDOW_SESSION",session,48))return 10;
 names(session);
 mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(*state),mapping_name);if(!mapping)return 2;
 state=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*state));if(!state)return 3;
 memset(state,0,sizeof(*state));state->magic=0x57575042;state->epoch=GetTickCount()|1;state->window_id=state->generation=1;state->width=320;state->height=240;
 acknowledge=CreateEventW(NULL,FALSE,FALSE,event_name);if(!acknowledge)return 4;
 cls.lpfnWndProc=guest_proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=guest_name;
 if(!RegisterClassW(&cls))return 5;
 window=CreateWindowExW(0,guest_name,guest_name,WS_POPUP|WS_VISIBLE,20,20,320,240,NULL,NULL,cls.hInstance,NULL);if(!window)return 6;
 original_proc=(WNDPROC)GetWindowLongPtrW(window,GWLP_WNDPROC);
 handler=AddVectoredExceptionHandler(1,guest_exception);if(!handler)return 11;
 for(unsigned iteration=0;iteration<3;iteration++){
 state->guest_calls=state->native_calls=state->guest_keys=state->error=0;
 state->guest_exceptions=state->native_exceptions=state->native_child_calls=state->guest_nulls=state->guest_builtins=state->native_builtins=0;
 state->generation=iteration+1;
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 state->driver_guest_ids=state->driver_attaches=state->driver_mirrors=state->driver_detaches=state->driver_rejects=0;
 driver_call=(driver_call_fn)GetProcAddress(GetModuleHandleW(L"win32u.dll"),"NtUserCallTwoParam");
 if(!driver_call)return 13;
 registration=(struct pw_d3d9_guest_window_request){.version=1,.size=sizeof(registration),.operation=PW_D3D9_GUEST_REGISTER};
 if(driver_call((ULONG_PTR)window,(ULONG_PTR)&registration,PW_D3D9_GUEST_WINDOW_CALL))return 14;
 state->epoch=registration.id.epoch;state->window_id=registration.id.id;state->generation=registration.id.generation;
 InterlockedIncrement(&state->driver_guest_ids);
 {struct pw_d3d9_window_driver_request invalid={.version=PW_D3D9_WINDOW_DRIVER_VERSION,.size=sizeof(invalid),.operation=PW_D3D9_WINDOW_ATTACH};
 if(driver_call((ULONG_PTR)&invalid,sizeof(invalid),PW_D3D9_WINDOW_DRIVER_CALL)!=PW_D3D9_WINDOW_INVALID)return 15;
 InterlockedIncrement(&state->driver_rejects);}
#endif
 state->width=320;state->height=240;state->create_hr=state->reset_hr=state->present_hr=0xdeadbeef;
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 if(!SetWindowPos(window,NULL,20,20,state->width,state->height,SWP_NOACTIVATE|SWP_NOZORDER))return 16;
#endif
 guest_builtin=builtin_create(window);if(!builtin_roundtrip(guest_builtin))return 12;
 InterlockedIncrement(&state->guest_builtins);
 SendMessageW(window,WM_NULL,0,0);RaiseException(0xe0425751,0,0,NULL);start=GetTickCount();
 memset(&request,0,sizeof(request));request.version=1;request.size=sizeof(request);MultiByteToWideChar(CP_UTF8,0,argv[1],-1,request.path,260);
 thread=CreateThread(NULL,0,broker,NULL,0,NULL);if(!thread)return 7;
 while(GetTickCount()-start<30000){
  DWORD wait=MsgWaitForMultipleObjects(1,&thread,FALSE,100,QS_ALLINPUT);
  if(wait==WAIT_OBJECT_0)break;
  while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
 }
 if(WaitForSingleObject(thread,0)!=WAIT_OBJECT_0)return 8;
 GetExitCodeThread(thread,&code);CloseHandle(thread);
#ifdef PW_BRIDGE_DRIVER_ASSOCIATION
 registration.operation=PW_D3D9_GUEST_UNREGISTER;
 if(driver_call((ULONG_PTR)window,(ULONG_PTR)&registration,PW_D3D9_GUEST_WINDOW_CALL))InterlockedExchange(&state->error,85);
 else InterlockedIncrement(&state->driver_guest_ids);
 printf("PW_BRIDGE_WINDOW_IDS iteration=%u guest=%ld attach=%ld mirror=%ld detach=%ld rejects=%ld\n",iteration,state->driver_guest_ids,state->driver_attaches,state->driver_mirrors,state->driver_detaches,state->driver_rejects);fflush(stdout);
 if(state->driver_guest_ids!=2 || state->driver_attaches!=1 || state->driver_mirrors!=2 || state->driver_detaches!=1 || state->driver_rejects!=3)InterlockedExchange(&state->error,86);
#endif
 {HWND after=builtin_create(window);
 if(!builtin_roundtrip(guest_builtin) || !builtin_roundtrip(after))InterlockedExchange(&state->error,84);
 if(after)DestroyWindow(after);
 DestroyWindow(guest_builtin);guest_builtin=NULL;
 InterlockedIncrement(&state->guest_builtins);}
 SendMessageW(window,WM_NULL,0,0);RaiseException(0xe0425751,0,0,NULL);
 printf("PW_BRIDGE_WINDOW iteration=%u status=%08lx error=%ld guest=%ld native=%ld keys=%ld proc64=%llx create=%08x reset=%08x present=%08x guest_exceptions=%ld native_exceptions=%ld native_child=%ld guest_nulls=%ld guest_builtins=%ld native_builtins=%ld\n",iteration,query_status,state->error,state->guest_calls,state->native_calls,state->guest_keys,request.result[1],state->create_hr,state->reset_hr,state->present_hr,state->guest_exceptions,state->native_exceptions,state->native_child_calls,state->guest_nulls,state->guest_builtins,state->native_builtins);fflush(stdout);
 code=code || state->error || state->guest_calls!=4 || !state->native_calls || state->guest_keys!=1 || request.result[1]<=UINT32_MAX || state->guest_exceptions!=3 || state->native_exceptions!=2 || state->native_child_calls!=1 || state->guest_nulls<2 || state->guest_builtins!=3 || state->native_builtins!=2;
 if(GetWindowLongPtrW(window,GWLP_WNDPROC)!=(LONG_PTR)original_proc)code=9;
 if(code)break;
 }
 fprintf(stderr,"PW_WINDOW_GUEST final=before-remove-handler code=%lu\n",code);fflush(stderr);
 RemoveVectoredExceptionHandler(handler);
 fprintf(stderr,"PW_WINDOW_GUEST final=before-destroy\n");fflush(stderr);
 DestroyWindow(window);
 fprintf(stderr,"PW_WINDOW_GUEST final=after-destroy\n");fflush(stderr);
 UnregisterClassW(guest_name,cls.hInstance);
 UnmapViewOfFile(state);state=NULL;CloseHandle(acknowledge);CloseHandle(mapping);
 fprintf(stderr,"PW_WINDOW_GUEST final=return code=%lu\n",code);fflush(stderr);
 return code;
}
#endif
