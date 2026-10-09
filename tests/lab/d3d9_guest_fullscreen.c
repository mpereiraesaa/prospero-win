/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#define PW_D3D9_ENABLE_DEVICE
#include "../../wine/ps5/d3d9/pw_d3d9_device_proxy.c"
#include <assert.h>
#include <string.h>
static unsigned notices;static int destroy_on_change;
static struct device_proxy *callback_device;static unsigned callback_checked,peer_calls,cancels;static HRESULT peer_hr;
HRESULT pw_d3d9_session_device(struct pw_d3d9_session *session,struct pw_d3d9_object_ref object,const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r)
{(void)session;(void)object;++peer_calls;*r=(struct pw_d3d9_device_reply){.operation=q->operation,.hresult=(uint32_t)peer_hr,.parameters=q->parameters};return peer_hr;}
void pw_d3d9_session_cancel(struct pw_d3d9_session *s){(void)s;++cancels;}
HRESULT pw_d3d9_session_join(struct pw_d3d9_session *s){(void)s;return S_OK;}
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *s,struct pw_d3d9_object_ref r){(void)s;(void)r;assert(0);return E_FAIL;}
HRESULT pw_d3d9_session_defer(struct pw_d3d9_session *s,struct pw_d3d9_deferred *d){(void)s;(void)d;assert(0);return E_FAIL;}

static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b)
{
 if(m==WM_WINDOWPOSCHANGED){
  notices++;
  if(callback_device){
   struct device_proxy *d=callback_device;callback_device=NULL;
   D3DPRESENT_PARAMETERS pp={.Windowed=FALSE,.hDeviceWindow=w};unsigned before=peer_calls;
   assert(reset(&d->iface,&pp)==RPC_E_CANTCALLOUT_ININPUTSYNCCALL&&peer_calls==before);
   assert(release(&d->iface)==1);assert(addref(&d->iface)==2);callback_checked++;
  }
 }
 if(m==WM_WINDOWPOSCHANGING&&destroy_on_change){destroy_on_change=0;DestroyWindow(w);return 0;}
 return DefWindowProcW(w,m,a,b);
}
static HWND hidden(void)
{
 RECT r={0,0,640,480};assert(AdjustWindowRectEx(&r,WS_OVERLAPPEDWINDOW,FALSE,0));
 return CreateWindowExW(0,L"PW-GuestFullscreen",L"guest hidden extent",WS_OVERLAPPEDWINDOW,
  50,70,r.right-r.left,r.bottom-r.top,NULL,NULL,GetModuleHandleW(NULL),NULL);
}
static int camera_extent(HWND w,LONG width,LONG height)
{RECT r;return GetClientRect(w,&r)&&r.right>=width&&r.bottom>=height;}
struct thread_test {struct pw_d3d9_guest_fullscreen *state;HWND window;HRESULT hr;};
static DWORD WINAPI wrong_thread(void *p)
{struct thread_test *t=p;RECT r={0,0,1920,1080};t->hr=pw_d3d9_guest_fullscreen_apply(t->state,t->window,FALSE,&r);return 0;}
int main(void)
{
 WNDCLASSW c={.lpfnWndProc=proc,.hInstance=GetModuleHandleW(NULL),.lpszClassName=L"PW-GuestFullscreen"};assert(RegisterClassW(&c));
 for(unsigned cycle=0;cycle<3;cycle++){
  HWND w=hidden();assert(w);struct pw_d3d9_guest_fullscreen state={0};RECT bounds={0,0,1920,1080},rect;
  LONG style=GetWindowLongW(w,GWL_STYLE),exstyle=GetWindowLongW(w,GWL_EXSTYLE);
  assert(!IsWindowVisible(w)&&camera_extent(w,640,480)&&!camera_extent(w,1920,1080));
  struct thread_test t={&state,w,S_OK};HANDLE thread=CreateThread(NULL,0,wrong_thread,&t,0,NULL);assert(thread);assert(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);CloseHandle(thread);assert(t.hr==D3DERR_INVALIDCALL&&!state.fullscreen);
  assert(pw_d3d9_guest_fullscreen_apply(&state,w,FALSE,&bounds)==S_OK);
  assert(camera_extent(w,1920,1080)&&IsWindowVisible(w)&&state.fullscreen);
  assert(state.style==style&&state.exstyle==exstyle);
  assert(!(GetWindowLongW(w,GWL_STYLE)&WS_OVERLAPPEDWINDOW));
  assert(GetWindowLongW(w,GWL_EXSTYLE)&WS_EX_TOPMOST);
  bounds=(RECT){0,0,1280,720};assert(pw_d3d9_guest_fullscreen_apply(&state,w,FALSE,&bounds)==S_OK);
  assert(camera_extent(w,1280,720)&&state.style==style);assert(GetWindowRect(w,&rect));
  assert(pw_d3d9_guest_fullscreen_apply(&state,w,TRUE,NULL)==S_OK&&!state.fullscreen);
  RECT after;assert(GetWindowRect(w,&after)&&!memcmp(&rect,&after,sizeof(rect)));
  assert(GetWindowLongW(w,GWL_STYLE)==style&&GetWindowLongW(w,GWL_EXSTYLE)==exstyle);
  assert(pw_d3d9_guest_fullscreen_apply(&state,w,FALSE,&bounds)==S_OK);
  LONG app_style=GetWindowLongW(w,GWL_STYLE)|WS_BORDER;SetWindowLongW(w,GWL_STYLE,app_style);
  assert(pw_d3d9_guest_fullscreen_apply(&state,w,TRUE,NULL)==S_OK);
  assert(GetWindowLongW(w,GWL_STYLE)==app_style); /* application changes survive */
  assert(pw_d3d9_guest_fullscreen_update(&state,w,FALSE)==S_OK);
  MONITORINFO mi={.cbSize=sizeof(mi)};POINT zero={0,0};assert(GetMonitorInfoW(MonitorFromPoint(zero,MONITOR_DEFAULTTOPRIMARY),&mi));
  assert(camera_extent(w,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top));
  assert(DestroyWindow(w));
  w=hidden();assert(w);memset(&state,0,sizeof(state));destroy_on_change=1;
  assert(FAILED(pw_d3d9_guest_fullscreen_apply(&state,w,FALSE,&bounds))&&!state.fullscreen&&!IsWindow(w));
  w=hidden();assert(w);struct guest_window gw={.guest=w,.id={1,1,1}};
  struct device_proxy d={.references=1,.window=&gw};D3DPRESENT_PARAMETERS pp={.Windowed=FALSE,.hDeviceWindow=w};
  assert(GetWindowRect(w,&rect));peer_hr=D3DERR_DEVICELOST;
  assert(reset(&d.iface,&pp)==D3DERR_DEVICELOST&&!d.fullscreen.fullscreen&&!IsWindowVisible(w));
  assert(GetWindowRect(w,&after)&&!memcmp(&rect,&after,sizeof(rect)));
  peer_hr=S_OK;callback_device=&d;
  assert(reset(&d.iface,&pp)==S_OK&&d.references==1&&!d.window_transition&&d.fullscreen.fullscreen);
  assert(callback_device==NULL&&callback_checked==cycle+1);
  pp.Windowed=TRUE;assert(reset(&d.iface,&pp)==S_OK&&!d.fullscreen.fullscreen);
  pp.Windowed=FALSE;destroy_on_change=1;unsigned canceled=cancels;
  assert(FAILED(reset(&d.iface,&pp))&&d.failed&&cancels==canceled+1&&!IsWindow(w)&&d.references==1);
  printf("PW_GUEST_FULLSCREEN cycle=%u hidden_negative=1 full_extent=1 reset=1 conditional_restore=1 destroyed_callback=1\n",cycle);
 }
 assert(notices);assert(UnregisterClassW(c.lpszClassName,c.hInstance));return 0;
}
