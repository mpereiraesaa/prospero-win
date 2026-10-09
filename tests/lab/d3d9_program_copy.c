/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include <windows.h>
static BOOL WINAPI __attribute__((unused)) denied_write(HANDLE,void *,const void *,SIZE_T,SIZE_T *);
#define WriteProcessMemory denied_write
#define PW_D3D9_ENABLE_DEVICE
#define PW_D3D9_ENABLE_PROGRAM
#include "../../wine/ps5/d3d9/pw_d3d9_device_proxy.c"
#undef WriteProcessMemory
#include <assert.h>
static unsigned denied,cancelled,mid_read_failure;
static const unsigned char declaration[]={0,0,0,0,2,0,0,0,255,0,0,0,17,0,0,0};
static unsigned char shader[5000];
static BOOL WINAPI __attribute__((unused)) denied_write(HANDLE process,void *out,const void *in,SIZE_T n,SIZE_T *written)
{(void)process;(void)out;(void)in;(void)n;denied++;if(written)*written=0;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
void pw_d3d9_session_cancel(struct pw_d3d9_session *s){(void)s;cancelled++;}
HRESULT pw_d3d9_session_join(struct pw_d3d9_session *s){(void)s;return S_OK;}
HRESULT pw_d3d9_session_defer(struct pw_d3d9_session *s,struct pw_d3d9_deferred *d){(void)s;(void)d;assert(0);return E_FAIL;}
HRESULT pw_d3d9_session_release(struct pw_d3d9_session *s,struct pw_d3d9_object_ref r){(void)s;(void)r;return S_OK;}
HRESULT pw_d3d9_session_device(struct pw_d3d9_session *s,struct pw_d3d9_object_ref o,const struct pw_d3d9_device_request *q,struct pw_d3d9_device_reply *r)
{(void)s;(void)o;(void)q;(void)r;return E_NOTIMPL;}
HRESULT pw_d3d9_session_program(struct pw_d3d9_session *s,struct pw_d3d9_object_ref o,const struct pw_d3d9_program_request *q,struct pw_d3d9_program_reply *r)
{(void)s;(void)o;(void)q;(void)r;return E_NOTIMPL;}
HRESULT pw_d3d9_session_program_query(struct pw_d3d9_session *s,struct pw_d3d9_object_ref o,const struct pw_d3d9_program_query_request *q,struct pw_d3d9_program_query_reply *r)
{
 unsigned char request[24],wire[4128];size_t bytes;struct pw_d3d9_program_query_request decoded;
 (void)s;assert(o.id==1&&o.generation==1);assert(!pw_d3d9_program_query_encode(request,sizeof(request),&bytes,q));assert(!pw_d3d9_program_query_decode(&decoded,request,bytes));
 struct pw_d3d9_program_query_reply native={.operation=q->operation,.kind=q->kind,.hresult=S_OK,.total=q->kind==7?sizeof(declaration):sizeof(shader),.size=q->kind==7?2:sizeof(shader)};
 if(mid_read_failure&&q->operation==PW_D3D9_PROGRAM_READ&&q->offset){native.hresult=E_FAIL;native.total=0;native.size=q->capacity;}
 else if(q->operation==PW_D3D9_PROGRAM_READ){
  UINT available=q->kind==7?native.total:(q->capacity<native.total?q->capacity:native.total);
  native.size=q->kind==7?2:q->capacity;native.offset=q->offset;native.count=available-q->offset;if(native.count>q->count)native.count=q->count;
  memcpy(native.data,(q->kind==7?declaration:shader)+q->offset,native.count);
 }
 assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&bytes,&decoded,&native));assert(!pw_d3d9_program_query_reply_decode(r,q,wire,bytes));return (HRESULT)r->hresult;
}
int main(int argc,char **argv)
{
 int old=argc==2&&!strcmp(argv[1],"--expect-denied");assert(argc==1||old);
 IDirect3DDevice9Vtbl table={.AddRef=addref,.Release=release};struct device_proxy d={.iface={&table},.references=1};
 const struct pw_d3d9_program_proxy_ops callbacks={program_call,program_query,release_object,defer_object,fail_device};pw_d3d9_program_proxy_install(&table,&callbacks);
 for(unsigned i=0;i<sizeof(shader);i++)shader[i]=(unsigned char)(i^(i>>8));
 const UINT capacities[]={0,1,2,17,4096,5000,8192,0xdeadbeef,0xffffffff};
 for(UINT kind=7;kind<=9;kind++){
  IUnknown *object=NULL;assert(pw_d3d9_program_proxy_wrap(&d.iface,kind,(struct pw_d3d9_object_ref){1,1},(void **)&object)==S_OK);
  for(unsigned i=0;i<sizeof(capacities)/sizeof(capacities[0]);i++){
   unsigned char out[8192];memset(out,0xa5,sizeof(out));UINT count=capacities[i],required=0;HRESULT hr;
   if(kind==7){assert(IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)object,NULL,&required)==S_OK&&required==2);hr=IDirect3DVertexDeclaration9_GetDeclaration((IDirect3DVertexDeclaration9 *)object,(void *)out,&count);}
   else if(kind==8){assert(IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)object,NULL,&required)==S_OK&&required==sizeof(shader));hr=IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)object,out,&count);}
   else{assert(IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)object,NULL,&required)==S_OK&&required==sizeof(shader));hr=IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)object,out,&count);}
   UINT bytes=kind==7?sizeof(declaration):(capacities[i]<sizeof(shader)?capacities[i]:sizeof(shader));
   assert(hr==(old&&bytes?D3DERR_INVALIDCALL:S_OK));
   if(old&&bytes){assert(count==capacities[i]);for(unsigned n=0;n<sizeof(out);n++)assert(out[n]==0xa5);}
   else{assert(count==(kind==7?2:capacities[i]));assert(!memcmp(out,kind==7?declaration:shader,bytes));for(unsigned n=bytes;n<sizeof(out);n++)assert(out[n]==0xa5);}
  }
  if(kind!=7){
   unsigned char untouched[8192];memset(untouched,0x6b,sizeof(untouched));UINT size=sizeof(untouched);mid_read_failure=1;
   HRESULT failed=kind==8?IDirect3DVertexShader9_GetFunction((IDirect3DVertexShader9 *)object,untouched,&size):IDirect3DPixelShader9_GetFunction((IDirect3DPixelShader9 *)object,untouched,&size);
   assert(failed==E_FAIL&&size==sizeof(untouched));for(unsigned n=0;n<sizeof(untouched);n++)assert(untouched[n]==0x6b);mid_read_failure=0;
  }
  assert(IUnknown_Release(object)==0);
 }
 assert(d.references==1&&!cancelled);assert(old?denied>0:denied==0);
 printf("PROGRAM_COPY PASS denied_control=%d denied_calls=%u kinds=3 capacities=9 multichunk=1 mid_error_unchanged=1 parent_refs=1\n",old,denied);return 0;
}
