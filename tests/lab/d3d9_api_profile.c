/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "../../wine/ps5/d3d9/pw_d3d9_api_observe.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PROFILE_DLL
static IDirect3DDevice9 device;
static IDirect3DSurface9 surface;
static unsigned transactions;
static void (*callback)(IDirect3DDevice9 *);
static ULONG WINAPI addref(IDirect3DDevice9 *self){(void)self;return 2;}
/* Deliberate internal sibling call: observer must retain its own frame. */
static ULONG WINAPI release(IDirect3DDevice9 *self){return IDirect3DDevice9_AddRef(self);}
static UINT WINAPI memory(IDirect3DDevice9 *self){(void)self;return 123;}
static HRESULT WINAPI query(IDirect3DDevice9 *self,REFIID iid,void **out)
{(void)self;(void)iid;if(out)*out=NULL;return E_NOINTERFACE;}
static HRESULT WINAPI validate(IDirect3DDevice9 *self,DWORD *passes)
{
 assert(IDirect3DDevice9_AddRef(self)==2);
 if(callback)callback(self);
 if(passes)*passes=7;
 assert(IDirect3DDevice9_Release(self)==2);
 errno=ERANGE;SetLastError(0xbeef);return D3DERR_INVALIDCALL;
}
static HRESULT WINAPI lock_rect(IDirect3DSurface9 *self,D3DLOCKED_RECT *out,const RECT *rect,DWORD flags)
{(void)self;(void)rect;(void)flags;transactions+=3;if(out)*out=(D3DLOCKED_RECT){16,(void *)0x1234};return S_OK;}
static void WINAPI position(IDirect3DDevice9 *self,int x,int y,DWORD flags)
{(void)self;assert(x==-2&&y==3&&flags==5);}
static BOOL WINAPI show(IDirect3DDevice9 *self,BOOL value)
{(void)self;return value;}
static float WINAPI patch(IDirect3DDevice9 *self){(void)self;return 3.25f;}
__declspec(dllexport) IDirect3DDevice9 *get_device(void (*cb)(IDirect3DDevice9 *),int *wrapped)
{
 static const IDirect3DDevice9Vtbl raw={.QueryInterface=query,.AddRef=addref,.Release=release,.ValidateDevice=validate,.GetAvailableTextureMem=memory,.SetCursorPosition=position,.ShowCursor=show,.GetNPatchMode=patch};
 callback=cb;device.lpVtbl=(IDirect3DDevice9Vtbl *)pw_d3d9_api_observe_IDirect3DDevice9(&raw);*wrapped=device.lpVtbl!=&raw;return &device;
}
__declspec(dllexport) IDirect3DSurface9 *get_surface(void)
{static const IDirect3DSurface9Vtbl raw={.LockRect=lock_rect};surface.lpVtbl=(IDirect3DSurface9Vtbl *)pw_d3d9_api_observe_IDirect3DSurface9(&raw);return &surface;}
__declspec(dllexport) unsigned get_transactions(void){return transactions;}
__declspec(dllexport) void snapshot(struct pw_d3d9_api_profile_snapshot *s){pw_d3d9_api_profile_snapshot(s);}
__declspec(dllexport) void flush(void){pw_d3d9_api_profile_flush(3);}
__declspec(dllexport) void present(HRESULT hr){pw_d3d9_api_profile_present(3,17,5,7,hr);}
#else
static void guest_callback(IDirect3DDevice9 *d){assert(IDirect3DDevice9_GetAvailableTextureMem(d)==123);}
static uint64_t total(const struct pw_d3d9_api_profile_snapshot *s)
{uint64_t n=0;for(unsigned i=0;i<PW_D3D9_API_INTERFACES;i++)for(unsigned j=0;j<PW_D3D9_API_SLOTS;j++)n+=s->entries[i][j];return n;}
#define LOAD(type,name) type name;do{FARPROC p=GetProcAddress(dll,#name);assert(p);memcpy(&name,&p,sizeof(name));}while(0)
int main(int argc,char **argv)
{
 assert(argc==4);int profile=atoi(argv[2]),diagnostics=atoi(argv[3]);
 SetEnvironmentVariableA("PW_D3D9_PROFILE",profile?"1":"0");SetEnvironmentVariableA("PW_D3D9_DIAGNOSTICS",diagnostics?"1":"0");
 HMODULE dll=LoadLibraryA(argv[1]);assert(dll);
 typedef IDirect3DDevice9 *(*device_fn)(void (*)(IDirect3DDevice9 *),int *);
 typedef IDirect3DSurface9 *(*surface_fn)(void);
 typedef unsigned (*count_fn)(void);typedef void (*snapshot_fn)(struct pw_d3d9_api_profile_snapshot *);typedef void (*present_fn)(HRESULT);typedef void (*flush_fn)(void);
 LOAD(device_fn,get_device);LOAD(surface_fn,get_surface);LOAD(count_fn,get_transactions);LOAD(snapshot_fn,snapshot);LOAD(present_fn,present);LOAD(flush_fn,flush);
 int wrapped=-1;IDirect3DDevice9 *d=get_device(guest_callback,&wrapped);IDirect3DSurface9 *s=get_surface();
 assert(wrapped==(profile||diagnostics));struct pw_d3d9_api_profile_snapshot before,after;snapshot(&before);assert(!total(&before));
 D3DLOCKED_RECT lock;assert(IDirect3DSurface9_LockRect(s,&lock,NULL,0)==S_OK&&lock.Pitch==16);
 snapshot(&after);assert(total(&after)==(profile?1:0)&&get_transactions()==3);
 DWORD passes=0;assert(IDirect3DDevice9_ValidateDevice(d,&passes)==D3DERR_INVALIDCALL&&passes==7&&errno==ERANGE&&GetLastError()==0xbeef);
 /* Validate + guest callback count, its two DLL-local reference pins do not. */
 snapshot(&after);assert(total(&after)==(profile?3:0));
 assert(IDirect3DDevice9_AddRef(d)==2);assert(IDirect3DDevice9_Release(d)==2);
 void *out=(void *)1;assert(IDirect3DDevice9_QueryInterface(d,NULL,&out)==E_NOINTERFACE&&!out);
 IDirect3DDevice9_SetCursorPosition(d,-2,3,5);assert(IDirect3DDevice9_ShowCursor(d,(BOOL)0x81234567)==(BOOL)0x81234567);assert(IDirect3DDevice9_GetNPatchMode(d)==3.25f);
 snapshot(&after);assert(total(&after)==(profile?9:0));assert(after.enabled==profile);assert(!profile||after.classification_valid);
 errno=ERANGE;SetLastError(0xbeef);present(E_FAIL);assert(errno==ERANGE&&GetLastError()==0xbeef);present(S_OK);
 for(unsigned i=3;i<=120;i++){assert(IDirect3DDevice9_GetAvailableTextureMem(d)==123);present(S_OK);}
 assert(IDirect3DDevice9_GetAvailableTextureMem(d)==123);flush();
 printf("PW_API_PROFILE profile=%d diagnostics=%d raw=%d entries=%llu transactions=%u callback=1 internal_pins_excluded=1 status=0\n",profile,diagnostics,!wrapped,(unsigned long long)total(&after),get_transactions());FreeLibrary(dll);return 0;
}
#endif
