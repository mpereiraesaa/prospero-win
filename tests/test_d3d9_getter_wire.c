/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_getter_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define COBJMACROS
#include <d3d9.h>
#define ABI(slot,name,args,bytes,element) _Static_assert(offsetof(IDirect3DDevice9Vtbl,name)==slot*sizeof(void *),"getter slot");
PW_D3D9_GETTER_METHODS(ABI)
#undef ABI
#endif
#define ID(slot,name,args,bytes,element) slot,
static const unsigned methods[]={PW_D3D9_GETTER_METHODS(ID)};
#undef ID
int main(void)
{
 unsigned char wire[PW_D3D9_GETTER_MAX],before_wire[PW_D3D9_GETTER_MAX];
 struct pw_d3d9_getter_request q={0},decoded,before_q;
 struct pw_d3d9_getter_reply r={0},out,before;
 size_t m,i,n,bytes;const struct pw_d3d9_getter_schema *schema;
 memset(&before,0xa5,sizeof(before));memset(&before_q,0x5a,sizeof(before_q));
 for(m=0;m<sizeof(methods)/sizeof(*methods);m++){
  q=(struct pw_d3d9_getter_request){.method=methods[m]};schema=pw_d3d9_getter_schema(q.method);assert(schema);
  if(schema->args)q.args[0]=0xfffffff0;
  if(schema->args==2)q.args[1]=schema->element?3:7;
  assert(!pw_d3d9_getter_bytes(&q,&bytes));
  assert(!pw_d3d9_getter_encode(wire,sizeof(wire),&n,&q)&&n==24);
  assert(!pw_d3d9_getter_decode(&decoded,wire,n)&&!memcmp(&q,&decoded,sizeof(q)));
  for(i=0;i<24;i++){decoded=before_q;assert(pw_d3d9_getter_decode(&decoded,wire,i));assert(!memcmp(&decoded,&before_q,sizeof(decoded)));}
  wire[20]=1;assert(pw_d3d9_getter_decode(&decoded,wire,24));wire[20]=0;
  r=(struct pw_d3d9_getter_reply){.method=q.method,.hresult=1,.bytes=bytes};
  for(i=0;i<bytes/4;i++)r.data.words[i]=(uint32_t)(0xffc00080u+i); /* NaN, signed and noncanonical true BOOL preserved */
  assert(!pw_d3d9_getter_reply_encode(wire,sizeof(wire),&n,&q,&r)&&n==bytes+16);
  assert(!pw_d3d9_getter_reply_decode(&out,&q,wire,n)&&!memcmp(&out,&r,sizeof(r)));
  for(i=0;i<n;i++){out=before;assert(pw_d3d9_getter_reply_decode(&out,&q,wire,i));assert(!memcmp(&out,&before,sizeof(out)));}
  memset(wire,0xac,sizeof(wire));memcpy(before_wire,wire,sizeof(wire));n=999;
  assert(pw_d3d9_getter_reply_encode(wire,15,&n,&q,&r)==PW_D3D9_GETTER_SMALL&&n==999&&!memcmp(wire,before_wire,sizeof(wire)));
  r.hresult=0x8876086c;r.bytes=0;
  assert(!pw_d3d9_getter_reply_encode(wire,sizeof(wire),&n,&q,&r)&&n==16);
  assert(!pw_d3d9_getter_reply_decode(&out,&q,wire,n)&&out.hresult==r.hresult&&!out.bytes);
  q.method=999;assert(pw_d3d9_getter_reply_decode(&out,&q,wire,n));
 }
 q=(struct pw_d3d9_getter_request){.method=95,.args={0,256}};assert(!pw_d3d9_getter_bytes(&q,&bytes)&&bytes==4096);
 q.args[1]=257;bytes=99;assert(pw_d3d9_getter_bytes(&q,&bytes)==PW_D3D9_GETTER_UNSUPPORTED&&bytes==99);
 q.args[1]=UINT32_MAX;assert(pw_d3d9_getter_bytes(&q,&bytes));
 q.args[1]=0;assert(!pw_d3d9_getter_bytes(&q,&bytes)&&bytes==0);
 q=(struct pw_d3d9_getter_request){.method=4,.args={1,0}};assert(pw_d3d9_getter_bytes(&q,&bytes));
 printf("PASS %u getter schemas, ABI slots, exact values, bounds and atomic replies\n",(unsigned)(sizeof(methods)/sizeof(*methods)));return 0;
}
