/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_object_getter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define COBJMACROS
#include <d3d9.h>
#define ABI(slot,name,args,kind,extra) _Static_assert(offsetof(IDirect3DDevice9Vtbl,name)==slot*sizeof(void *),"object getter slot");
PW_D3D9_OBJECT_GETTERS(ABI)
#undef ABI
#endif
#define ID(slot,name,args,kind,extra) slot,
static const unsigned methods[]={PW_D3D9_OBJECT_GETTERS(ID)};
#undef ID
int main(void)
{
 unsigned char wire[32],before_wire[32];struct pw_d3d9_object_getter_request q={0},decoded,before_q;
 struct pw_d3d9_object_getter_reply r={0},out,before;size_t i,m,n;
 memset(&before,0xa5,sizeof(before));memset(&before_q,0xa6,sizeof(before_q));
 for(m=0;m<sizeof(methods)/sizeof(*methods);m++){
  const struct pw_d3d9_object_getter_schema *s=pw_d3d9_object_getter_schema(methods[m]);assert(s);
  q=(struct pw_d3d9_object_getter_request){.method=methods[m]};for(i=0;i<s->args;i++)q.args[i]=(uint32_t)(i+1);
  assert(!pw_d3d9_object_getter_encode(wire,sizeof(wire),&n,&q)&&n==24);
  assert(!pw_d3d9_object_getter_decode(&decoded,wire,n)&&!memcmp(&q,&decoded,sizeof(q)));
  for(i=0;i<24;i++){decoded=before_q;assert(pw_d3d9_object_getter_decode(&decoded,wire,i)&&!memcmp(&decoded,&before_q,sizeof(decoded)));}
  wire[20]=1;assert(pw_d3d9_object_getter_decode(&decoded,wire,24));
  r=(struct pw_d3d9_object_getter_reply){.method=q.method,.kind=s->kind,.id=5,.generation=7};
  if(s->extra){r.offset=UINT32_MAX;r.stride=0x80000000;}
  assert(!pw_d3d9_object_getter_reply_encode(wire,sizeof(wire),&n,&q,&r)&&n==32);
  assert(!pw_d3d9_object_getter_reply_decode(&out,&q,wire,n)&&!memcmp(&out,&r,sizeof(r)));
  for(i=0;i<32;i++){out=before;assert(pw_d3d9_object_getter_reply_decode(&out,&q,wire,i)&&!memcmp(&out,&before,sizeof(out)));}
  memset(wire,0xc5,sizeof(wire));memcpy(before_wire,wire,sizeof(wire));n=999;
  assert(pw_d3d9_object_getter_reply_encode(wire,31,&n,&q,&r)==PW_D3D9_OBJECT_GETTER_SMALL&&n==999&&!memcmp(wire,before_wire,sizeof(wire)));
  r.kind++;assert(pw_d3d9_object_getter_reply_encode(wire,sizeof(wire),&n,&q,&r));r.kind=s->kind;
  r.generation=0;assert(pw_d3d9_object_getter_reply_encode(wire,sizeof(wire),&n,&q,&r));
  r.kind=r.id=0;assert(!pw_d3d9_object_getter_reply_encode(wire,sizeof(wire),&n,&q,&r));
  assert(!pw_d3d9_object_getter_reply_decode(&out,&q,wire,n)&&!out.id&&!out.kind&&!out.generation);
  r.hresult=0x8876086c;r.offset=r.stride=0;assert(!pw_d3d9_object_getter_reply_encode(wire,sizeof(wire),&n,&q,&r));
  assert(!pw_d3d9_object_getter_reply_decode(&out,&q,wire,n)&&out.hresult==r.hresult);
  q.method=999;assert(pw_d3d9_object_getter_reply_decode(&out,&q,wire,n));
 }
 puts("PASS nine object getter schemas, ABI slots, kinds, nulls, exact errors and atomic replies");return 0;
}
