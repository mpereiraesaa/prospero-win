/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_stateblock_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
 unsigned methods[]={4,5,59,60,61};unsigned char wire[17],before[17];
 for(unsigned i=0;i<5;i++){
  struct pw_d3d9_stateblock_request q={methods[i],methods[i]==59?3:0},decoded={0};
  struct pw_d3d9_stateblock_reply r={0x1234,{0}},result;
  assert(!pw_d3d9_stateblock_encode(wire,16,&q));assert(wire[0]==1 && wire[4]==methods[i]);
  assert(!pw_d3d9_stateblock_decode(&decoded,wire,16) && !memcmp(&q,&decoded,sizeof(q)));
  for(size_t n=0;n<17;n++)if(n!=16){struct pw_d3d9_stateblock_request x=decoded;assert(pw_d3d9_stateblock_decode(&decoded,wire,n));assert(!memcmp(&decoded,&x,sizeof(x)));}
  if(q.method==59 || q.method==61)r.object=(struct pw_d3d9_object_ref){0x12345678,0x87654321};
  assert(!pw_d3d9_stateblock_reply_encode(wire,16,&q,&r));assert(!pw_d3d9_stateblock_reply_decode(&result,&q,wire,16));assert(!memcmp(&r,&result,sizeof(r)));
  r.hresult=0x8876086c;r.object=(struct pw_d3d9_object_ref){0};assert(!pw_d3d9_stateblock_reply_encode(wire,16,&q,&r));assert(!pw_d3d9_stateblock_reply_decode(&result,&q,wire,16));assert(result.hresult==r.hresult);
  memset(wire,0xa5,sizeof(wire));memcpy(before,wire,sizeof(wire));r.object.id=1;
  assert(pw_d3d9_stateblock_reply_encode(wire,16,&q,&r));assert(!memcmp(wire,before,sizeof(wire)));
 }
 {struct pw_d3d9_stateblock_request q={60,1};assert(pw_d3d9_stateblock_validate(&q)==PW_D3D9_SB_INVALID);q.method=0;assert(pw_d3d9_stateblock_validate(&q)==PW_D3D9_SB_UNSUPPORTED);q.method=59;q.type=~0u;assert(!pw_d3d9_stateblock_validate(&q));}
 {struct pw_d3d9_stateblock_request q={59,1},x={0};assert(!pw_d3d9_stateblock_encode(wire,16,&q));wire[12]=1;assert(pw_d3d9_stateblock_decode(&x,wire,16));assert(!x.method);wire[12]=0;wire[0]=2;assert(pw_d3d9_stateblock_decode(&x,wire,16));}
 puts("PASS stateblock codec:5 methods, exact HRESULT, typed creation refs, atomic invalid payloads");return 0;
}
