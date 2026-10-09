/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_program_query.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void)
{
 unsigned char wire[32+PW_D3D9_PROGRAM_CHUNK],saved[sizeof(wire)];size_t n,i;
 struct pw_d3d9_program_query_request q={0},decoded,before_q;
 struct pw_d3d9_program_query_reply r={0},out,before;
 memset(&before,0xa5,sizeof(before));memset(&before_q,0xa6,sizeof(before_q));
 for(unsigned kind=7;kind<=9;kind++){
  q=(struct pw_d3d9_program_query_request){PW_D3D9_PROGRAM_SIZE,kind,123,0,0};
  assert(!pw_d3d9_program_query_encode(wire,sizeof(wire),&n,&q)&&n==24);
  assert(!pw_d3d9_program_query_decode(&decoded,wire,n)&&!memcmp(&q,&decoded,sizeof(q)));
  for(i=0;i<24;i++){decoded=before_q;assert(pw_d3d9_program_query_decode(&decoded,wire,i));assert(!memcmp(&decoded,&before_q,sizeof(decoded)));}
  r=(struct pw_d3d9_program_query_reply){.operation=q.operation,.kind=kind,.total=kind==7?24:5000,.size=kind==7?3:5000};
  assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&n,&q,&r)&&n==32);
  assert(!pw_d3d9_program_query_reply_decode(&out,&q,wire,n)&&out.size==r.size);
  q.operation=PW_D3D9_PROGRAM_READ;q.capacity=kind==7?0:UINT32_MAX;q.count=4096;
  r.operation=q.operation;r.count=kind==7?24:4096;r.size=kind==7?3:UINT32_MAX;memset(r.data,0x4b,sizeof(r.data));
  assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&n,&q,&r));
  assert(!pw_d3d9_program_query_reply_decode(&out,&q,wire,n)&&out.count==r.count&&!memcmp(out.data,r.data,r.count));
  for(i=0;i<n;i++){out=before;assert(pw_d3d9_program_query_reply_decode(&out,&q,wire,i));assert(!memcmp(&out,&before,sizeof(out)));}
  memcpy(saved,wire,n);wire[28]++;out=before;assert(pw_d3d9_program_query_reply_decode(&out,&q,wire,n));assert(!memcmp(&out,&before,sizeof(out)));memcpy(wire,saved,n);
  if(kind!=7){q.capacity=3;r.count=3;r.size=3;assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&n,&q,&r));q.capacity=0;r.count=0;r.size=0;assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&n,&q,&r));}
  r.hresult=0x8876086c;r.total=r.offset=r.count=0;r.size=17;assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&n,&q,&r));assert(!pw_d3d9_program_query_reply_decode(&out,&q,wire,n)&&out.size==17&&out.hresult==r.hresult);
 }
 q=(struct pw_d3d9_program_query_request){PW_D3D9_PROGRAM_READ,8,5000,4096,904};
 r=(struct pw_d3d9_program_query_reply){.operation=2,.kind=8,.size=5000,.total=5000,.offset=4096,.count=904};
 assert(!pw_d3d9_program_query_reply_encode(wire,sizeof(wire),&n,&q,&r));
 q.offset=UINT32_MAX;n=99;memset(wire,0xaf,sizeof(wire));memcpy(saved,wire,sizeof(wire));assert(pw_d3d9_program_query_encode(wire,sizeof(wire),&n,&q));assert(n==99&&!memcmp(wire,saved,sizeof(wire)));
 q.offset=0;q.count=4097;assert(pw_d3d9_program_query_encode(wire,sizeof(wire),&n,&q));q.count=0;q.kind=10;assert(pw_d3d9_program_query_encode(wire,sizeof(wire),&n,&q));
 puts("program query wire passed");return 0;
}
