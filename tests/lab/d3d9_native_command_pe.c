/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include "pw_d3d9_command_wire.h"
#include "pw_d3d9_command_policy.h"
#ifdef _WIN64
#include "pw_d3d9_native_command.h"
#endif
#define CAPACITY 64
struct shared {uint32_t magic,count,length[CAPACITY],hr[CAPACITY];unsigned char wire[CAPACITY][PW_D3D9_COMMAND_MAX];};
static WCHAR map_name[96];
static int name(void){WCHAR id[48];
 if(!GetEnvironmentVariableW(L"PW_COMMAND_SESSION",id,48))return 0;
 swprintf(map_name,96,L"Local\\PW-Commands-%ls",id);return 1;}
#ifdef _WIN64
struct objects {IDirect3DDevice9 *device;IUnknown *entries[5];unsigned pins,releases;};
static HRESULT acquire(void *context,uint32_t id,uint32_t generation,uint32_t kind,IDirect3DDevice9 *device,void **out)
{
 struct objects *o=context;static const unsigned kinds[]={0,PW_D3D9_KIND_SURFACE,PW_D3D9_KIND_TEXTURE_2D,PW_D3D9_KIND_VERTEX_BUFFER,PW_D3D9_KIND_INDEX_BUFFER};
 if(id>=5 || !id || generation!=1 || kind!=kinds[id] || device!=o->device || !o->entries[id])return D3DERR_INVALIDCALL;
 IUnknown_AddRef(o->entries[id]);*out=o->entries[id];o->pins++;return S_OK;
}
static HRESULT direct_policy(IDirect3DDevice9 *device,const struct pw_d3d9_command *c)
{
 switch(c->method){
 case 44:return IDirect3DDevice9_SetTransform(device,c->args[0],(const D3DMATRIX *)c->data.bytes);
 case 57:return IDirect3DDevice9_SetRenderState(device,c->args[0],c->args[1]);
 case 67:return IDirect3DDevice9_SetTextureStageState(device,c->args[0],c->args[1],c->args[2]);
 case 69:return IDirect3DDevice9_SetSamplerState(device,c->args[0],c->args[1],c->args[2]);
 case 47:return IDirect3DDevice9_SetViewport(device,(const D3DVIEWPORT9 *)c->data.bytes);
 case 49:return IDirect3DDevice9_SetMaterial(device,(const D3DMATERIAL9 *)c->data.bytes);
 case 75:return IDirect3DDevice9_SetScissorRect(device,(const RECT *)c->data.bytes);
 default:return E_FAIL;
 }
}
static int policy_case(IDirect3DDevice9 *device,struct pw_d3d9_command *c,int eligible,unsigned *count)
{
 struct pw_d3d9_command decoded;unsigned char wire[PW_D3D9_COMMAND_MAX];size_t written;
 if(pw_d3d9_command_can_queue(c)!=eligible || pw_d3d9_command_encode(wire,sizeof(wire),&written,c) ||
 pw_d3d9_command_decode(&decoded,wire,written) || pw_d3d9_command_can_queue(&decoded)!=eligible)return 0;
 HRESULT direct=direct_policy(device,c),actual=pw_d3d9_native_command_dispatch(device,&decoded,NULL,NULL);
 if(actual!=direct || (eligible && actual!=S_OK))return 0;
 ++*count;return 1;
}
static HRESULT direct_constant(IDirect3DDevice9 *device,unsigned method,UINT start,const void *data,UINT count)
{
 switch(method){
 case 94:return IDirect3DDevice9_SetVertexShaderConstantF(device,start,data,count);
 case 96:return IDirect3DDevice9_SetVertexShaderConstantI(device,start,data,count);
 case 98:return IDirect3DDevice9_SetVertexShaderConstantB(device,start,data,count);
 case 109:return IDirect3DDevice9_SetPixelShaderConstantF(device,start,data,count);
 case 111:return IDirect3DDevice9_SetPixelShaderConstantI(device,start,data,count);
 default:return IDirect3DDevice9_SetPixelShaderConstantB(device,start,data,count);
 }
}
static int constant_policy_proof(IDirect3DDevice9 *device)
{
 D3DDEVICE_CREATION_PARAMETERS creation;IDirect3DStateBlock9 *saved=NULL,*recorded=NULL;
 const unsigned methods[]={94,96,98,109,111,113};uint32_t values[1024]={0};unsigned comparisons=0;int ok=0;
 if(FAILED(IDirect3DDevice9_GetCreationParameters(device,&creation)) ||
 FAILED(IDirect3DDevice9_CreateStateBlock(device,D3DSBT_ALL,&saved)))return 0;
 for(unsigned record=0;record<2;record++){
  if(record && FAILED(IDirect3DDevice9_BeginStateBlock(device)))goto done;
  for(unsigned m=0;m<6;m++){
   unsigned vertex=m<3,floating=m==0||m==3;
   UINT software=vertex?(floating?8192u:2048u):(floating?224u:16u);
   UINT hardware=vertex&&!(creation.BehaviorFlags&0xa0)?(floating?256u:16u):software;
   struct edge {UINT start,count;int present;} edges[]={
    {0,0,0},{0,1,0},{software,0,0},{software+1,0,0},{UINT32_MAX,1,1},
    {hardware-1,2,1},{hardware,1,0},{software-1,1,1},{0,1,1},{0,256,1},{0,0,1}};
   for(unsigned i=0;i<sizeof(edges)/sizeof(edges[0]);i++){
    struct edge e=edges[i];UINT effective=999;
    int plan=pw_d3d9_command_constant_count(methods[m],e.start,e.count,creation.BehaviorFlags,e.present,&effective);
    HRESULT direct=direct_constant(device,methods[m],e.start,e.present?values:NULL,e.count);
    if(plan<0){if(direct!=D3DERR_INVALIDCALL || effective!=999)goto done;}
    else{
     struct pw_d3d9_command c={.method=methods[m],.args={e.start,effective}};
     c.data_bytes=effective*((m==2||m==5)?4:16);
     if(plan!=1 || c.data_bytes>sizeof(values) || !pw_d3d9_command_can_queue(&c) || direct!=S_OK ||
      pw_d3d9_native_command_dispatch(device,&c,NULL,NULL)!=direct)goto done;
    }
    ++comparisons;
   }
  }
  if(record && FAILED(IDirect3DDevice9_EndStateBlock(device,&recorded)))goto done;
 }
 ok=comparisons==132;
 done:
 if(recorded)IDirect3DStateBlock9_Release(recorded);
 if(saved){if(FAILED(IDirect3DStateBlock9_Apply(saved)))ok=0;IDirect3DStateBlock9_Release(saved);}
 printf("PW_CONSTANT_POLICY creation=%08lx comparisons=%u live_and_recorded=1 ok=%d\n",creation.BehaviorFlags,comparisons,ok);
 return ok;
}
static int policy_proof(IDirect3DDevice9 *device)
{
 IDirect3DStateBlock9 *saved=NULL,*recorded=NULL;struct pw_d3d9_command c;unsigned count=0;int ok=0;
 const unsigned types[]={1,2,3,4,5,6,7,8,9,10,11,22,23,24,26,27,28,32};
 if(FAILED(IDirect3DDevice9_CreateStateBlock(device,D3DSBT_ALL,&saved)))return 0;
 for(unsigned record=0;record<2;record++){
  if(record && FAILED(IDirect3DDevice9_BeginStateBlock(device)))goto done;
  c=(struct pw_d3d9_command){.method=57,.args={D3DRS_ZENABLE,0}};
  if(!policy_case(device,&c,1,&count))goto done;
  c.args[0]=0xffffffff;if(!policy_case(device,&c,1,&count))goto done;
  for(unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++){
   c=(struct pw_d3d9_command){.method=67,.args={i&1?0xffffffff:0,types[i],1}};
   if(!policy_case(device,&c,1,&count))goto done;
  }
  for(unsigned slot=0;slot<=260;slot++)if(slot<16||slot>=256)for(unsigned type=1;type<=13;type++){
   c=(struct pw_d3d9_command){.method=69,.args={slot,type,1}};
   if(!policy_case(device,&c,1,&count))goto done;
  }
  c=(struct pw_d3d9_command){.method=69,.args={16,1,1}};
  if(!policy_case(device,&c,0,&count))goto done; /* Backend no-op, conservative fallback. */
  c.args[0]=0xffffffff;if(!policy_case(device,&c,0,&count))goto done;
  c=(struct pw_d3d9_command){.method=47,.data_bytes=24};
  {D3DVIEWPORT9 value={0,0,64,64,0,1};memcpy(c.data.bytes,&value,sizeof(value));}
  if(!policy_case(device,&c,1,&count))goto done;
  c=(struct pw_d3d9_command){.method=49,.data_bytes=68};
  if(!policy_case(device,&c,1,&count))goto done;
  c=(struct pw_d3d9_command){.method=75,.data_bytes=16};
  {RECT value={0,0,64,64};memcpy(c.data.bytes,&value,sizeof(value));}
  if(!policy_case(device,&c,1,&count))goto done;
 {const unsigned states[]={2,3,16,23,256,511};
 for(unsigned i=0;i<6;i++){
  c=(struct pw_d3d9_command){.method=44,.args={states[i]},.data_bytes=64};
  D3DMATRIX value={0};value._11=value._22=value._33=value._44=1;memcpy(c.data.bytes,&value,sizeof(value));
  if(!policy_case(device,&c,1,&count))goto done;
  if(IDirect3DDevice9_SetTransform(device,states[i],NULL)!=S_OK ||
   pw_d3d9_native_command_dispatch(device,&c,NULL,NULL)!=S_OK)goto done;
  ++count;
 }}
  if(record && FAILED(IDirect3DDevice9_EndStateBlock(device,&recorded)))goto done;
 }
 if(IDirect3DDevice9_SetMaterial(device,NULL)!=D3DERR_INVALIDCALL ||
 IDirect3DDevice9_SetScissorRect(device,NULL)!=D3DERR_INVALIDCALL)goto done;
 ok=count==620;
 done:
 if(recorded)IDirect3DStateBlock9_Release(recorded);
 if(saved){if(FAILED(IDirect3DStateBlock9_Apply(saved)))ok=0;IDirect3DStateBlock9_Release(saved);}
 printf("PW_COMMAND_POLICY cases=%u live_and_recorded=1 fallback=4 null_rejects=2 ok=%d\n",count,ok);
 return ok;
}
static LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b){return DefWindowProcW(w,m,a,b);}
__declspec(dllexport) DWORD WINAPI PwD3D9ServiceMain(uint64_t *result)
{
 HANDLE mapping=NULL;struct shared *s=NULL;WCHAR path[260];HMODULE backend=NULL;IDirect3D9 *d3d=NULL;
 IDirect3D9 *(WINAPI *factory)(UINT);IDirect3DDevice9 *device=NULL;D3DPRESENT_PARAMETERS pp={0};WCHAR behavior_text[32];DWORD behavior=D3DCREATE_HARDWARE_VERTEXPROCESSING;
 IDirect3DSurface9 *surface=NULL,*readback=NULL;IDirect3DTexture9 *texture=NULL;IDirect3DVertexBuffer9 *vb=NULL;IDirect3DIndexBuffer9 *ib=NULL;
 WNDCLASSW cls={0};HWND window=NULL;DWORD error=1;struct objects objects={0};void *data;
 struct vertex {float x,y,z,w;DWORD color;} vertices[3]={{4,4,0.5f,1,0xffff0000},{60,4,0.5f,1,0xff00ff00},{4,60,0.5f,1,0xff0000ff}};
 WORD indices[3]={0,1,2};struct pw_d3d9_command c;HRESULT hr;DWORD fvf;
 if(*(volatile LONG *)((char *)NtCurrentTeb()+0x180c) || !name())return 2;
 mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,map_name);
 if(!mapping)goto done;
 s=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*s));
 if(!s || s->magic!=0x434d4439 || s->count>CAPACITY)goto done;
 cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"PW-NativeCommand";
 if(!RegisterClassW(&cls))goto done;
 window=CreateWindowExW(0,cls.lpszClassName,L"command proof",WS_POPUP,0,0,64,64,NULL,NULL,cls.hInstance,NULL);
 if(!window)goto done;
 if(!GetEnvironmentVariableW(L"PW_COMMAND_BACKEND",path,260) || !(backend=LoadLibraryW(path)))goto done;
 factory=(void *)GetProcAddress(backend,"Direct3DCreate9");
 if(!factory || !(d3d=factory(D3D_SDK_VERSION)))goto done;
 pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.BackBufferWidth=pp.BackBufferHeight=64;pp.hDeviceWindow=window;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 if(GetEnvironmentVariableW(L"PW_COMMAND_BEHAVIOR",behavior_text,32))behavior=wcstoul(behavior_text,NULL,0);
 hr=IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,behavior,&pp,&device);
 result[0]=(uint32_t)hr;
 if(FAILED(hr))goto done;
 if(!policy_proof(device) || !constant_policy_proof(device))goto done;
 if(FAILED(IDirect3DDevice9_GetRenderTarget(device,0,&surface)) || FAILED(IDirect3DDevice9_CreateTexture(device,4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,NULL)) ||
 FAILED(IDirect3DDevice9_CreateVertexBuffer(device,sizeof(vertices),0,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,D3DPOOL_MANAGED,&vb,NULL)) ||
 FAILED(IDirect3DDevice9_CreateIndexBuffer(device,sizeof(indices),0,D3DFMT_INDEX16,D3DPOOL_MANAGED,&ib,NULL)))goto done;
 if(FAILED(IDirect3DVertexBuffer9_Lock(vb,0,0,&data,0)))goto done;
 memcpy(data,vertices,sizeof(vertices));IDirect3DVertexBuffer9_Unlock(vb);
 if(FAILED(IDirect3DIndexBuffer9_Lock(ib,0,0,&data,0)))goto done;
 memcpy(data,indices,sizeof(indices));IDirect3DIndexBuffer9_Unlock(ib);
 objects.device=device;objects.entries[1]=(IUnknown *)surface;objects.entries[2]=(IUnknown *)texture;objects.entries[3]=(IUnknown *)vb;objects.entries[4]=(IUnknown *)ib;
 IDirect3DDevice9_SetRenderState(device,D3DRS_LIGHTING,FALSE);IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLOROP,D3DTOP_SELECTARG2);IDirect3DDevice9_SetTextureStageState(device,0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
 for(unsigned i=0;i<s->count;i++){
  if(pw_d3d9_command_decode(&c,s->wire[i],s->length[i]))goto done;
  hr=pw_d3d9_native_command_dispatch(device,&c,acquire,&objects);s->hr[i]=(uint32_t)hr;
  printf("PW_COMMAND_NATIVE index=%u method=%u hr=%08lx\n",i,c.method,hr);
  if(i>=40 && FAILED(hr))goto done;
 }
 {unsigned methods[]={94,96,98,109,111,113};float f[4]={0};int integers[4]={0};BOOL booleans[1]={0};
 for(unsigned i=0;i<6;i++){
  HRESULT direct,actual;memset(&c,0,sizeof(c));c.method=methods[i];
  switch(c.method){
  case 94:direct=IDirect3DDevice9_SetVertexShaderConstantF(device,0,f,0);break;
  case 96:direct=IDirect3DDevice9_SetVertexShaderConstantI(device,0,integers,0);break;
  case 98:direct=IDirect3DDevice9_SetVertexShaderConstantB(device,0,booleans,0);break;
  case 109:direct=IDirect3DDevice9_SetPixelShaderConstantF(device,0,f,0);break;
  case 111:direct=IDirect3DDevice9_SetPixelShaderConstantI(device,0,integers,0);break;
  default:direct=IDirect3DDevice9_SetPixelShaderConstantB(device,0,booleans,0);break;
  }
  actual=pw_d3d9_native_command_dispatch(device,&c,acquire,&objects);
  printf("PW_COMMAND_ZERO method=%u direct=%08lx helper=%08lx\n",c.method,direct,actual);
  if(actual!=direct)goto done;
 }
 result[5]=6;
 }
 if(FAILED(IDirect3DDevice9_GetFVF(device,&fvf)) || fvf!=(D3DFVF_XYZRHW|D3DFVF_DIFFUSE))goto done;
 memset(&c,0,sizeof(c));c.method=57;c.args[0]=0xffffffff;c.args[1]=123;
 if(pw_d3d9_native_command_dispatch(device,&c,acquire,&objects)!=IDirect3DDevice9_SetRenderState(device,(D3DRENDERSTATETYPE)0xffffffff,123))goto done;
 {D3DLOCKED_RECT locked;uint32_t pixel;
 if(FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device,64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,NULL)) ||
 FAILED(IDirect3DDevice9_GetRenderTargetData(device,surface,readback)) || FAILED(IDirect3DSurface9_LockRect(readback,&locked,NULL,D3DLOCK_READONLY)))goto done;
 memcpy(&pixel,(unsigned char *)locked.pBits+16*locked.Pitch+16*4,4);IDirect3DSurface9_UnlockRect(readback);result[4]=pixel;
 if((pixel&0xffffff)==0x305080)goto done;
 }
 hr=IDirect3DDevice9_Present(device,NULL,NULL,NULL,NULL);
 result[1]=(uint32_t)hr;
 if(FAILED(hr))goto done;
 result[2]=s->count;
 result[3]=objects.pins;error=0;
 done:
 if(device){IDirect3DDevice9_SetTexture(device,0,NULL);IDirect3DDevice9_SetStreamSource(device,0,NULL,0,0);IDirect3DDevice9_SetIndices(device,NULL);}
 if(readback)IDirect3DSurface9_Release(readback);
 if(ib)IDirect3DIndexBuffer9_Release(ib);
 if(vb)IDirect3DVertexBuffer9_Release(vb);
 if(texture)IDirect3DTexture9_Release(texture);
 if(surface)IDirect3DSurface9_Release(surface);
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
static uint32_t bits(float f){uint32_t u;
 memcpy(&u,&f,4);return u;}
static int append(struct shared *s,unsigned method)
{
 struct pw_d3d9_command c={0};size_t bytes,written;unsigned i;c.method=method;
 switch(method){
 case 37:c.args[1]=1;c.args[2]=1;break;
 case 43:c.args[1]=D3DCLEAR_TARGET;c.args[2]=0xff305080;c.args[3]=bits(1);break;
 case 44:case 46:c.args[0]=D3DTS_WORLD;for(i=0;i<4;i++)c.data.words[5*i]=bits(1);break;
 case 47:c.data.words[2]=c.data.words[3]=64;c.data.words[5]=bits(1);break;
 case 49:for(i=0;i<4;i++)c.data.words[i]=bits(1);break;
 case 51:c.data.words[0]=D3DLIGHT_POINT;c.data.words[19]=bits(100);c.data.words[21]=bits(1);break;
 case 55:c.data.words[0]=bits(1);break;
 case 57:c.args[0]=D3DRS_ZENABLE;c.args[1]=FALSE;break;
 case 65:c.args[1]=2;c.args[2]=1;break;
 case 67:c.args[1]=D3DTSS_COLOROP;c.args[2]=D3DTOP_SELECTARG2;break;
 case 69:c.args[1]=D3DSAMP_MINFILTER;c.args[2]=D3DTEXF_POINT;break;
 case 75:c.data.words[2]=c.data.words[3]=64;break;
 case 81:c.args[0]=D3DPT_TRIANGLELIST;c.args[2]=1;break;
 case 82:c.args[0]=D3DPT_TRIANGLELIST;c.args[3]=3;c.args[5]=1;break;
 case 89:c.args[0]=D3DFVF_XYZRHW|D3DFVF_DIFFUSE;break;
 case 94:case 96:case 98:case 109:case 111:case 113:c.args[1]=1;break;
 case 100:c.args[1]=3;c.args[2]=1;c.args[4]=20;break;
 case 102:c.args[1]=1;break;
 case 104:c.args[0]=4;c.args[1]=1;break;
 }
 if(pw_d3d9_command_data_bytes(method,c.args,&bytes))return 0;
 c.data_bytes=bytes;
 if(s->count>=CAPACITY || pw_d3d9_command_encode(s->wire[s->count],PW_D3D9_COMMAND_MAX,&written,&c))return 0;
 s->length[s->count++]=written;return 1;
}
int main(int argc,char **argv)
{
 struct request r={0};HANDLE mapping;struct shared *s;ULONG returned;LONG status;query_fn query;int result;
 if(argc!=2 || !name())return 1;
 mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,sizeof(*s),map_name);
 if(!mapping)return 2;
 s=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(*s));
 if(!s)return 3;
 memset(s,0,sizeof(*s));s->magic=0x434d4439;
#define APPEND(slot,name,words,shape,objects,booleans) if(!append(s,slot))return 4;
 PW_D3D9_COMMAND_METHODS(APPEND)
#undef APPEND
 /* Repeat a valid draw sequence after all bindings/state have been exercised. */
 {unsigned order[]={89,100,104,43,41,81,82,42};for(unsigned i=0;i<8;i++)if(!append(s,order[i]))return 5;}
 r.version=1;r.size=sizeof(r);MultiByteToWideChar(CP_UTF8,0,argv[1],-1,r.path,260);
 query=(query_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
 if(!query)return 6;
 status=query(GetCurrentProcess(),0x50570001,&r,sizeof(r),&returned);
 printf("PW_COMMAND_PE status=%08lx create=%08llx present=%08llx commands=%llu pins=%llu pixel=%08llx zero=%llu error=%llu\n",status,r.result[0],r.result[1],r.result[2],r.result[3],r.result[4],r.result[5],r.result[7]);
 result=s->hr[25]!=(uint32_t)D3DERR_INVALIDCALL || s->hr[26]!=(uint32_t)D3DERR_INVALIDCALL || status || returned!=sizeof(r) || r.result[0] || r.result[1] || r.result[2]!=48 || r.result[5]!=6 || r.result[7];UnmapViewOfFile(s);CloseHandle(mapping);return result;
}
#endif
