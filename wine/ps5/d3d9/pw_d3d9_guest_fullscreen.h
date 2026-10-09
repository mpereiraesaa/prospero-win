/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_D3D9_GUEST_FULLSCREEN_H
#define PW_D3D9_GUEST_FULLSCREEN_H
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
/* Guest-local window state, never transmitted. DXVK owns display mode changes
 * and the native WNDPROC; this supplies its missing guest HWND side effects. */
struct pw_d3d9_guest_fullscreen { LONG style,exstyle; BOOL fullscreen; };
static inline BOOL pw_guest_set_style(HWND hwnd,int index,LONG value)
{SetLastError(0);return SetWindowLongW(hwnd,index,value)!=0||GetLastError()==0;}
static inline HRESULT pw_d3d9_guest_fullscreen_apply(struct pw_d3d9_guest_fullscreen *s,
 HWND hwnd,BOOL windowed,const RECT *bounds)
{
 RECT before,client,after;LONG style,exstyle;BOOL was_visible;char text[256];
 if(!s||!IsWindow(hwnd)||GetWindowThreadProcessId(hwnd,NULL)!=GetCurrentThreadId())return D3DERR_INVALIDCALL;
 if(windowed&&!s->fullscreen)return S_OK;
 if(!windowed&&(!bounds||bounds->right<=bounds->left||bounds->bottom<=bounds->top))return D3DERR_INVALIDCALL;
 if(!GetWindowRect(hwnd,&before)||!GetClientRect(hwnd,&client))return E_FAIL;
 style=GetWindowLongW(hwnd,GWL_STYLE);exstyle=GetWindowLongW(hwnd,GWL_EXSTYLE);was_visible=!!(style&WS_VISIBLE);
 struct pw_d3d9_guest_fullscreen next=*s;
 if(!windowed){
  if(!s->fullscreen){
   next.style=style;next.exstyle=exstyle;
   if(!pw_guest_set_style(hwnd,GWL_STYLE,style&~WS_OVERLAPPEDWINDOW)||
      !pw_guest_set_style(hwnd,GWL_EXSTYLE,exstyle&~WS_EX_OVERLAPPEDWINDOW))goto failed;
  }
  if(!SetWindowPos(hwnd,HWND_TOPMOST,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,
                   SWP_FRAMECHANGED|SWP_SHOWWINDOW|SWP_NOACTIVATE))goto failed;
 }else{
  if((style&~WS_VISIBLE)==(s->style&~(WS_VISIBLE|WS_OVERLAPPEDWINDOW))&&
     (exstyle&~WS_EX_TOPMOST)==(s->exstyle&~(WS_EX_TOPMOST|WS_EX_OVERLAPPEDWINDOW))){
   if(!pw_guest_set_style(hwnd,GWL_STYLE,s->style)||!pw_guest_set_style(hwnd,GWL_EXSTYLE,s->exstyle))goto failed;
  }
  if(!SetWindowPos(hwnd,s->exstyle&WS_EX_TOPMOST?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
                   SWP_FRAMECHANGED|SWP_NOACTIVATE|SWP_NOSIZE|SWP_NOMOVE))goto failed;
 }
 if(!IsWindow(hwnd)||!GetClientRect(hwnd,&after))goto failed;
 next.fullscreen=!windowed;*s=next;
 snprintf(text,sizeof(text),"PW_BRIDGE_GUEST_WINDOW hwnd=%p fullscreen=%u before=%ldx%ld after=%ldx%ld visible=%u\n",
          (void *)hwnd,(unsigned)!windowed,client.right-client.left,client.bottom-client.top,
          after.right-after.left,after.bottom-after.top,(unsigned)IsWindowVisible(hwnd));OutputDebugStringA(text);
 return S_OK;
 failed:
 /* A callback may destroy the HWND. Never recreate it or publish partial state. */
 if(IsWindow(hwnd)){
  pw_guest_set_style(hwnd,GWL_STYLE,style);pw_guest_set_style(hwnd,GWL_EXSTYLE,exstyle);
  SetWindowPos(hwnd,exstyle&WS_EX_TOPMOST?HWND_TOPMOST:HWND_NOTOPMOST,before.left,before.top,
   before.right-before.left,before.bottom-before.top,SWP_FRAMECHANGED|SWP_NOACTIVATE|(was_visible?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
 }
 return E_FAIL;
}
static inline HRESULT pw_d3d9_guest_fullscreen_update(struct pw_d3d9_guest_fullscreen *s,HWND hwnd,BOOL windowed)
{
 MONITORINFO monitor={.cbSize=sizeof(monitor)};POINT origin={0,0};
 if(!windowed&&!GetMonitorInfoW(MonitorFromPoint(origin,MONITOR_DEFAULTTOPRIMARY),&monitor))return E_FAIL;
 return pw_d3d9_guest_fullscreen_apply(s,hwnd,windowed,windowed?NULL:&monitor.rcMonitor);
}
#endif
