/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include "pw_d3d9_getter_wire.h"
#ifdef _WIN64
#include "pw_d3d9_native_getter.h"
#endif
#define CAPACITY 40
struct shared {uint32_t magic,count,reply_bytes[CAPACITY];unsigned char requests[CAPACITY][24],replies[CAPACITY][PW_D3D9_GETTER_MAX];};
static WCHAR map_name[96];
static int name(void)
{
 WCHAR id[48];
 if(!GetEnvironmentVariableW(L"PW_GETTER_SESSION",id,48))return 0;
 swprintf(map_name,96,L"Local\\PW-Getters-%ls",id);return 1;
}
#ifdef _WIN64
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
 HANDLE mapping=NULL;struct shared *s=NULL;WCHAR path[260];HMODULE backend=NULL;IDirect3D9 *d3d=NULL;
 IDirect3D9 *(WINAPI *factory)(UINT);IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS pp={0};
 WNDCLASSW cls={0};HWND window=NULL;DWORD error=1;HRESULT hr;
 D3DMATRIX matrix={0};D3DVIEWPORT9 viewport={3,5,32,24,0.25f,0.75f};D3DMATERIAL9 material={0};D3DLIGHT9 light={0};
 D3DCLIPSTATUS9 clip={1,0};RECT rect={3,5,31,29};PALETTEENTRY palette[256];
 float constants[8]={1,2,3,4,5,6,7,8};int integers[8]={(-2147483647-1),-1,1,2147483647,16,17,18,19};BOOL booleans[2]={TRUE,FALSE};float plane[4]={1,0,0,-3};
 if(*(volatile LONG *)((char *)NtCurrentTeb()+0x180c) || !name())return 2;
 mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,map_name);
 if(!mapping)goto done;
 s=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*s));
 if(!s || s->magic!=0x47545239 || s->count>CAPACITY)goto done;
 cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW-NativeGetter";
 if(!RegisterClassW(&cls))goto done;
 window=CreateWindowExW(0,cls.lpszClassName,L"getter proof",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);
 if(!window)goto done;
 if(!GetEnvironmentVariableW(L"PW_GETTER_BACKEND",path,260) || !(backend=LoadLibraryW(path)))goto done;
 factory=(void *)GetProcAddress(backend,"Direct3DCreate9");
 if(!factory || !(d3d=factory(D3D_SDK_VERSION)))goto done;
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=pp.BackBufferHeight=64;pp.hDeviceWindow=window;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&device);result[0]=(uint32_t)hr;
 if(FAILED(hr))goto done;
 for(unsigned i=0;i<4;i++)matrix.m[i][i]=1;
 matrix.m[3][0]=-3;matrix.m[3][1]=4;matrix.m[3][2]=5;
 material.Diffuse.r=0.25f;material.Diffuse.g=0.5f;material.Diffuse.b=0.75f;material.Diffuse.a=1;material.Power=8;
 light.Type=D3DLIGHT_POINT;light.Diffuse.r=light.Diffuse.g=light.Diffuse.b=light.Diffuse.a=1;light.Position.x=1;light.Position.y=2;light.Position.z=3;light.Range=100;light.Attenuation0=1;
 for(unsigned i=0;i<256;i++){palette[i].peRed=i;palette[i].peGreen=255-i;palette[i].peBlue=17;palette[i].peFlags=0;}
#define SET(name,...) if(FAILED(IDirect3DDevice9_##name(device, __VA_ARGS__)))goto done
 SET(SetTransform,D3DTS_WORLD,&matrix);SET(SetViewport,&viewport);SET(SetMaterial,&material);SET(SetLight,7,&light);SET(LightEnable,7,TRUE);
 SET(SetClipPlane,2,plane);SET(SetRenderState,D3DRS_ZENABLE,FALSE);SET(SetClipStatus,&clip);SET(SetTextureStageState,0,D3DTSS_COLOROP,D3DTOP_SELECTARG2);
 SET(SetSamplerState,0,D3DSAMP_MINFILTER,D3DTEXF_POINT);SET(SetPaletteEntries,5,palette);SET(SetCurrentTexturePalette,5);SET(SetScissorRect,&rect);
 SET(SetSoftwareVertexProcessing,FALSE);SET(SetNPatchMode,0);SET(SetFVF,D3DFVF_XYZRHW|D3DFVF_DIFFUSE);SET(SetStreamSourceFreq,2,1);
 SET(SetVertexShaderConstantF,3,constants,2);SET(SetVertexShaderConstantI,3,integers,2);SET(SetVertexShaderConstantB,3,booleans,2);
 SET(SetPixelShaderConstantF,3,constants,2);SET(SetPixelShaderConstantI,3,integers,2);SET(SetPixelShaderConstantB,3,booleans,2);
#undef SET
 for(unsigned i=0;i<s->count;i++){
  struct pw_d3d9_getter_request q;struct pw_d3d9_getter_reply r;size_t written;
  if(pw_d3d9_getter_decode(&q,s->requests[i],24) || pw_d3d9_native_getter_dispatch(device,&q,&r) ||
     pw_d3d9_getter_reply_encode(s->replies[i],PW_D3D9_GETTER_MAX,&written,&q,&r))goto done;
  if(q.method==72 || q.method==74){
   UINT current=0;HRESULT direct=q.method==72?IDirect3DDevice9_GetPaletteEntries(device,q.args[0],palette):IDirect3DDevice9_GetCurrentTexturePalette(device,&current);
   if((uint32_t)direct!=r.hresult)goto done;
   printf("PW_GETTER_DIRECT method=%u hr=%08lx\n",q.method,direct);
  }
  s->reply_bytes[i]=(uint32_t)written;
  printf("PW_GETTER_NATIVE index=%u method=%u hr=%08x bytes=%u\n",i,q.method,r.hresult,r.bytes);
 }
 result[1]=s->count;error=0;
 done:
 if(device)IDirect3DDevice9_Release(device);
 if(d3d)IDirect3D9_Release(d3d);
 if(window)DestroyWindow(window);
 if(cls.lpszClassName)UnregisterClassW(cls.lpszClassName,cls.hInstance);
 if(backend)FreeLibrary(backend);
 if(s)UnmapViewOfFile(s);
 if(mapping)CloseHandle(mapping);
 result[7]=error;return error;
}
#else
struct request {ULONG version,size;WCHAR path[260];uint64_t result[8];};
typedef LONG (WINAPI *query_fn)(HANDLE,ULONG,void *,ULONG,ULONG *);
static uint32_t bits(float f){uint32_t u;memcpy(&u,&f,4);return u;}
static int append(struct shared *s,unsigned method,unsigned special)
{
 struct pw_d3d9_getter_request q={0};const struct pw_d3d9_getter_schema *schema=pw_d3d9_getter_schema(method);size_t written;q.method=method;
 switch(method){
 case 45:q.args[0]=D3DTS_WORLD;break;
 case 52:q.args[0]=special?999:7;break;
 case 54:q.args[0]=7;break;
 case 56:q.args[0]=2;break;
 case 58:q.args[0]=D3DRS_ZENABLE;break;
 case 66:q.args[1]=D3DTSS_COLOROP;break;
 case 68:q.args[1]=D3DSAMP_MINFILTER;break;
 case 72:q.args[0]=5;break;
 case 103:q.args[0]=2;break;
 }
 if(schema->element){q.args[0]=3;q.args[1]=special?0:2;}
 if(s->count>=CAPACITY || pw_d3d9_getter_encode(s->requests[s->count],24,&written,&q) || written!=24)return 0;
 s->count++;return 1;
}
static int validate(const struct pw_d3d9_getter_request *q,const struct pw_d3d9_getter_reply *r)
{
 const uint32_t *d=r->data.words;
 if(q->method==52 && q->args[0]==999)return r->hresult==(uint32_t)D3DERR_INVALIDCALL && !r->bytes;
 if(FAILED((HRESULT)r->hresult))return q->method==19 || q->method==70 || q->method==72 || q->method==74;
 switch(q->method){
 case 15:return d[0]==1;
 case 45:return d[0]==bits(1) && d[5]==bits(1) && d[10]==bits(1) && d[12]==bits(-3) && d[13]==bits(4) && d[14]==bits(5) && d[15]==bits(1);
 case 48:return d[0]==3 && d[1]==5 && d[2]==32 && d[3]==24 && d[4]==bits(0.25f) && d[5]==bits(0.75f);
 case 50:return d[0]==bits(0.25f) && d[1]==bits(0.5f) && d[2]==bits(0.75f) && d[3]==bits(1) && d[16]==bits(8);
 case 52:return d[0]==D3DLIGHT_POINT && d[13]==bits(1) && d[14]==bits(2) && d[15]==bits(3) && d[19]==bits(100);
 case 54:return !!d[0];
 case 56:return d[0]==bits(1) && d[1]==0 && d[2]==0 && d[3]==bits(-3);
 case 58:case 78:case 80:return d[0]==0;
 case 66:return d[0]==D3DTOP_SELECTARG2;
 case 68:return d[0]==D3DTEXF_POINT;
 case 74:return d[0]==5;
 case 76:return d[0]==3 && d[1]==5 && d[2]==31 && d[3]==29;
 case 90:return d[0]==(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
 case 103:return d[0]==1;
 case 95:case 110:return !q->args[1] || (d[0]==bits(1) && d[7]==bits(8));
 case 97:case 112:return !q->args[1] || (d[0]==0x80000000u && d[1]==0xffffffffu && d[3]==0x7fffffffu && d[7]==19);
 case 99:case 114:return !q->args[1] || (d[0] && !d[1]);
 default:return 1;
 }
}
int main(int argc,char **argv)
{
 struct request request={0};HANDLE mapping;struct shared *s;ULONG returned;LONG status;query_fn query;int result=1;
 if(argc!=2 || !name())return 1;
 mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(*s),map_name);
 if(!mapping)return 2;
 s=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*s));
 if(!s)return 3;
 memset(s,0,sizeof(*s));s->magic=0x47545239;
#define APPEND(slot,name,args,bytes,element) if(!append(s,slot,0))return 4;
 PW_D3D9_GETTER_METHODS(APPEND)
#undef APPEND
 if(!append(s,95,1) || !append(s,99,1) || !append(s,114,1) || !append(s,52,1))return 5;
 request.version=1;request.size=sizeof(request);MultiByteToWideChar(CP_UTF8,0,argv[1],-1,request.path,260);
 query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
 if(!query)return 6;
 status=query(GetCurrentProcess(),0x50570001,&request,sizeof(request),&returned);
 if(status || returned!=sizeof(request) || request.result[0] || request.result[1]!=32 || request.result[7])goto done;
 for(unsigned i=0;i<s->count;i++){
  struct pw_d3d9_getter_request q;struct pw_d3d9_getter_reply r;
  if(pw_d3d9_getter_decode(&q,s->requests[i],24) || pw_d3d9_getter_reply_decode(&r,&q,s->replies[i],s->reply_bytes[i]) || !validate(&q,&r)){printf("PW_GETTER_INVALID index=%u\n",i);goto done;}
 }
 result=0;
 done:
 printf("PW_GETTER_PE status=%08lx create=%08llx replies=%llu validated=%d error=%llu\n",status,request.result[0],request.result[1],result?0:32,request.result[7]);
 UnmapViewOfFile(s);CloseHandle(mapping);return result;
}
#endif
