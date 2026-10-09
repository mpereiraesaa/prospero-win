/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define main baseline_main
#include "d3d9_texture_proxy.c"
#undef main
#ifdef PW_D3D9_ENABLE_BINDING_TICKETS
static struct pw_d3d9_queue_ticket *drop_in_release;
static struct pw_d3d9_deferred *retained;
static unsigned releases;static int defer_failure;static HRESULT release_result;
static HRESULT ticket_release(IDirect3DDevice9 *d,struct pw_d3d9_object_ref ref)
{
 HRESULT hr=remote_release(d,ref);if(hr==S_OK)releases++;
 if(drop_in_release){struct pw_d3d9_queue_ticket *t=drop_in_release;drop_in_release=NULL;pw_d3d9_queue_ticket_drop(t);assert(parent_refs==2);}
 return FAILED(hr)?hr:release_result;
}
static HRESULT ticket_defer(IDirect3DDevice9 *d,struct pw_d3d9_deferred *n)
{if(defer_failure){retained=n;return E_OUTOFMEMORY;}return defer(d,n);}
int main(void)
{
 assert(!baseline_main()&&parent_refs==1);negative=0;
 IDirect3DDevice9Vtbl table={.AddRef=parent_addref,.Release=parent_release};parent.lpVtbl=&table;IDirect3DDevice9 foreign={&table};
 struct pw_d3d9_texture_proxy_ops ops={exchange,ticket_release,ticket_defer,fail};pw_d3d9_texture_proxy_install(&table,&ops);
 for(unsigned mode=0;mode<5;mode++){
  IDirect3DTexture9 *p=NULL;struct pw_d3d9_queue_ticket ticket={0},second={0};struct pw_d3d9_object_ref ref;
  refs[7]++;assert(pw_d3d9_texture_proxy_wrap(&parent,5,(struct pw_d3d9_object_ref){7,9},6,(void **)&p)==S_OK);
  assert(pw_d3d9_texture_proxy_ticket(&foreign,(IUnknown *)p,5,&ref,&ticket)==D3DERR_INVALIDCALL&&!ticket.drop);
  assert(pw_d3d9_texture_proxy_ticket(&parent,(void *)1,5,&ref,&ticket)==D3DERR_INVALIDCALL&&!ticket.drop);
  assert(pw_d3d9_texture_proxy_ticket(&parent,(IUnknown *)p,6,&ref,&ticket)==D3DERR_INVALIDCALL&&!ticket.drop);
  assert(pw_d3d9_texture_proxy_ticket(&parent,(IUnknown *)p,5,&ref,&ticket)==S_OK&&ref.id==7&&ticket.drop);
  assert(pw_d3d9_texture_proxy_ticket(&parent,(IUnknown *)p,5,&ref,&ticket)==D3DERR_INVALIDCALL);
  assert(pw_d3d9_texture_proxy_ticket(&parent,(IUnknown *)p,5,&ref,&second)==S_OK);
  pw_d3d9_queue_ticket_drop(&second);pw_d3d9_queue_ticket_drop(&second);
  D3DSURFACE_DESC description;assert(IDirect3DTexture9_GetLevelDesc(p,0,&description)==S_OK);
  D3DLOCKED_RECT map={0};contents[0]=0x5a;HRESULT lock_hr=IDirect3DTexture9_LockRect(p,0,&map,NULL,D3DLOCK_READONLY);
  if(sizeof(void *)==4)assert(lock_hr==S_OK&&map.pBits&&((unsigned char *)map.pBits)[0]==0x5a);else assert(lock_hr==E_OUTOFMEMORY&&!map.pBits);
  unsigned before=releases;release_result=mode==4?E_FAIL:S_OK;
  if(mode==1)drop_in_release=&ticket;
  blocked=mode==2||mode==3;defer_failure=mode==3;
  assert(IDirect3DTexture9_Release(p)==0);
  if(mode==1){assert(parent_refs==1&&!ticket.drop&&releases==before+1);continue;}
  assert(parent_refs==2&&(!map.pBits||((unsigned char *)map.pBits)[0]==0x5a));assert(IDirect3DTexture9_AddRef(p)==0);
  void *out=(void *)1;assert(IDirect3DTexture9_QueryInterface(p,&IID_IUnknown,&out)==D3DERR_INVALIDCALL&&!out);
  assert(pw_d3d9_texture_proxy_ticket(&parent,(IUnknown *)p,5,&ref,&second)==D3DERR_INVALIDCALL&&!second.drop);
  if(blocked){assert(releases==before);pw_d3d9_queue_ticket_drop(&ticket);assert(parent_refs==2&&(!map.pBits||((unsigned char *)map.pBits)[0]==0x5a));blocked=0;defer_failure=0;
   struct pw_d3d9_deferred *node=mode==2?pending:retained;pending=retained=NULL;node->function(node->context);
  }else{assert(releases==before+1);pw_d3d9_queue_ticket_drop(&ticket);}
  assert(parent_refs==1&&!refs[7]&&!active[7]&&!pw_d3d9_staging_bytes());
 }
 puts("PW_TEXTURE_TICKETS public_zero_barrier=1 finishing=1 deferred=1 terminal=1 parent_leaks=0 status=0");return 0;
}
#else
int main(void){return baseline_main();}
#endif
