/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define main baseline_main
#include "d3d9_program_proxy.c"
#undef main
static struct pw_d3d9_queue_ticket *drop_in_release;
static HRESULT release_result;
static HRESULT ticket_release(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref)
{
 HRESULT hr=remote_release(d,ref);
 if(drop_in_release){struct pw_d3d9_queue_ticket *t=drop_in_release;drop_in_release=NULL;pw_d3d9_queue_ticket_drop(t);assert(parent_refs==2);}
 return FAILED(hr)?hr:release_result;
}
int main(void)
{
 assert(!baseline_main());assert(parent_refs==1);
 IDirect3DDevice9Vtbl table={.AddRef=parent_addref,.Release=parent_release};IDirect3DDevice9 device={&table},foreign={&table};
 struct pw_d3d9_program_proxy_ops ops={call,query,ticket_release,defer,fail};pw_d3d9_program_proxy_install(&table,&ops);
 for(unsigned kind=7;kind<=9;kind++)for(unsigned mode=0;mode<5;mode++){
  IUnknown *p=NULL;struct pw_d3d9_queue_ticket ticket={0},second={0};struct pw_d3d9_object_ref ref;
  assert(pw_d3d9_program_proxy_wrap(&device,kind,(struct pw_d3d9_object_ref){kind,1},(void **)&p)==S_OK);
  assert(pw_d3d9_program_proxy_ticket(&foreign,p,kind,&ref,&ticket)==D3DERR_INVALIDCALL&&!ticket.drop);
  assert(pw_d3d9_program_proxy_ticket(&device,(void *)1,kind,&ref,&ticket)==D3DERR_INVALIDCALL&&!ticket.drop);
  assert(pw_d3d9_program_proxy_ticket(&device,p,kind==7?8:7,&ref,&ticket)==D3DERR_INVALIDCALL&&!ticket.drop);
  assert(pw_d3d9_program_proxy_ticket(&device,p,kind,&ref,&ticket)==S_OK&&ref.id==kind&&ticket.drop);
  assert(pw_d3d9_program_proxy_ticket(&device,p,kind,&ref,&ticket)==D3DERR_INVALIDCALL);
  assert(pw_d3d9_program_proxy_ticket(&device,p,kind,&ref,&second)==S_OK);
  pw_d3d9_queue_ticket_drop(&second);pw_d3d9_queue_ticket_drop(&second);
  unsigned before=releases;release_result=mode==4?E_FAIL:S_OK;
  if(mode==1)drop_in_release=&ticket;
  nested=mode==2||mode==3;defer_failure=mode==3;
  assert(IUnknown_Release(p)==0);
  if(mode==1){assert(parent_refs==1&&!ticket.drop&&releases==before+1);continue;}
  assert(parent_refs==2);assert(IUnknown_AddRef(p)==0);
  void *out=(void *)1;assert(IUnknown_QueryInterface(p,&IID_IUnknown,&out)==D3DERR_INVALIDCALL&&!out);
  assert(pw_d3d9_program_proxy_ticket(&device,p,kind,&ref,&second)==D3DERR_INVALIDCALL&&!second.drop);
  if(nested){assert(releases==before);pw_d3d9_queue_ticket_drop(&ticket);assert(parent_refs==2);nested=0;defer_failure=0;
   if(mode==2)drain();else{struct pw_d3d9_deferred *node=retained;retained=NULL;node->function(node->context);}
  }else{assert(releases==before+1);pw_d3d9_queue_ticket_drop(&ticket);}
  assert(parent_refs==1);
 }
 puts("PW_PROGRAM_TICKETS kinds=3 public_zero_barrier=1 finishing=1 deferred=1 terminal=1 parent_leaks=0 status=0");return 0;
}
