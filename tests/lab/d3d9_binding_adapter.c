/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define main baseline_main
#include "d3d9_device_methods.c"
#undef main
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
static HRESULT result;
static IUnknown *expected_local;
static unsigned expected_word,binding_calls;
static HRESULT binding_callback(IDirect3DDevice9 *d,struct pw_d3d9_command *c,IUnknown *local,uint32_t kind,uint32_t word)
{
 assert(d==owner&&local==expected_local&&kind==expected_kind&&word==expected_word);
 assert(c->method==method&&!c->data_bytes&&!c->args[word]&&!c->args[word+1]);
 for(unsigned i=0;i<6;i++)if(i!=word&&i!=word+1)assert(c->args[i]==args[i]);
 binding_calls++;return result;
}
int main(void)
{
 assert(!baseline_main());mode=0;device_refs=1;
 IDirect3DDevice9Vtbl table={0};IDirect3DDevice9 device={&table};owner=&device;
 struct pw_d3d9_device_methods_ops callbacks={command,getter,fail,resolve};pw_d3d9_device_methods_install(&table,&callbacks);
 pw_d3d9_device_methods_binding_install(binding_callback);
 const unsigned methods[]={65,87,92,100,104,107},kinds[]={5,7,8,3,4,9},words[]={1,0,0,1,0,0};
 const HRESULT results[]={S_OK,D3DERR_INVALIDCALL,RPC_E_CANTCALLOUT_ININPUTSYNCCALL,E_FAIL};
 unsigned before=resolves,commands=calls;
 for(unsigned n=0;n<6;n++)for(unsigned empty=0;empty<2;empty++)for(unsigned r=0;r<4;r++){
  setup(methods[n]);expected_kind=kinds[n];expected_word=words[n];expected_local=empty?NULL:object;result=results[r];HRESULT hr=E_UNEXPECTED;
  if(n==0){args[0]=260;hr=IDirect3DDevice9_SetTexture(&device,260,(void *)expected_local);}
  if(n==1)hr=IDirect3DDevice9_SetVertexDeclaration(&device,(void *)expected_local);
  if(n==2)hr=IDirect3DDevice9_SetVertexShader(&device,(void *)expected_local);
  if(n==3){args[0]=15;args[3]=args[4]=UINT32_MAX;hr=IDirect3DDevice9_SetStreamSource(&device,15,(void *)expected_local,UINT32_MAX,UINT32_MAX);}
  if(n==4)hr=IDirect3DDevice9_SetIndices(&device,(void *)expected_local);
  if(n==5)hr=IDirect3DDevice9_SetPixelShader(&device,(void *)expected_local);
  assert(hr==result);
 }
 assert(binding_calls==48&&resolves==before&&calls==commands);
 pw_d3d9_device_methods_binding_install(NULL);setup(92);expected_kind=8;args[0]=77;args[1]=88;
 assert(IDirect3DDevice9_SetVertexShader(&device,(void *)object)==0x1234&&resolves==before+1&&calls==commands+1);
 puts("PW_BINDING_ADAPTER methods=6 nulls=1 exact_hresult=1 atomic_callback=1 fallback=1 status=0");return 0;
}
#else
int main(void){return baseline_main();}
#endif
