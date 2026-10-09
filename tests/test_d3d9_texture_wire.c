/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_texture_wire.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void roundtrip(unsigned op)
{
 struct pw_d3d9_texture_request q={0},decoded,old;
 struct pw_d3d9_texture_reply r={0},reply,old_reply;
 unsigned char wire[PW_D3D9_TEXTURE_MAX_WIRE],copy[sizeof(wire)];size_t n,i,k;
 q.operation=op;q.width=128;q.height=64;q.levels=8;q.format=21;q.pool=1;q.usage=0x200;
 q.level=3;q.flags=0x3010;q.has_rect=1;q.left=4;q.top=8;q.right=20;q.bottom=40;
 q.has_point=1;q.x=-3;q.y=99;q.source=(struct pw_d3d9_object_ref){1,2};q.destination=(struct pw_d3d9_object_ref){3,4};
 q.lock_generation=UINT64_C(0x1234567887654321);q.offset=77;q.count=4096;
 for(i=0;i<q.count;i++)q.data[i]=(unsigned char)i;
 assert(!pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));
 assert(!pw_d3d9_texture_request_decode(&decoded,wire,n));
 assert(!pw_d3d9_texture_request_encode(copy,sizeof(copy),&k,&decoded));assert(k==n&&!memcmp(copy,wire,n));
 memset(&decoded,0x55,sizeof(decoded));old=decoded;
 for(i=0;i<n;i++){assert(pw_d3d9_texture_request_decode(&decoded,wire,i));assert(!memcmp(&decoded,&old,sizeof(old)));}
 wire[12]=1;assert(pw_d3d9_texture_request_decode(&decoded,wire,n));wire[12]=0;
 assert(pw_d3d9_texture_request_decode(&decoded,wire,n+1));
 r.operation=op;r.object=(struct pw_d3d9_object_ref){7,8};r.levels=8;
 r.desc=(struct pw_d3d9_surface_desc){21,1,0x200,1,0,0,128,64};
 r.lock_generation=UINT64_C(0xfedcba9876543210);r.pitch=-64;r.rows=3;r.row_bytes=48;r.length=176;
 r.count=4096;r.offset=9;memcpy(r.data,q.data,sizeof(r.data));
 for(unsigned failure=0;failure<2;failure++){
  r.hresult=failure?0x8876086a:0;
  assert(!pw_d3d9_texture_reply_encode(wire,sizeof(wire),&n,&r));
  assert(!pw_d3d9_texture_reply_decode(&reply,wire,n));assert(reply.hresult==r.hresult);
  if(failure)assert(n==16&&!reply.object.id&&!reply.lock_generation&&!reply.count);
  assert(!pw_d3d9_texture_reply_encode(copy,sizeof(copy),&k,&reply));assert(k==n&&!memcmp(copy,wire,n));
  memset(&reply,0xaa,sizeof(reply));old_reply=reply;
  for(i=0;i<n;i++){assert(pw_d3d9_texture_reply_decode(&reply,wire,i));assert(!memcmp(&reply,&old_reply,sizeof(reply)));}
 }
}
int main(void)
{
 struct pw_d3d9_texture_request q={.operation=PW_D3D9_TEXTURE_LOCK,.has_rect=2};
 unsigned char wire[PW_D3D9_TEXTURE_MAX_WIRE];size_t n;unsigned op;
 for(op=1;op<=12;op++)roundtrip(op);
 assert(pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));q.has_rect=0;q.left=1;
 assert(pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_UPDATE};assert(pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));
 q=(struct pw_d3d9_texture_request){.operation=PW_D3D9_TEXTURE_WRITE,.lock_generation=1,.offset=UINT32_MAX,.count=1};assert(pw_d3d9_texture_request_encode(wire,sizeof(wire),&n,&q));
 assert(pw_d3d9_texture_layout_valid(-64,3,48,176));assert(pw_d3d9_texture_layout_valid(64,3,48,176));
 assert(!pw_d3d9_texture_layout_valid(0,1,1,1));assert(!pw_d3d9_texture_layout_valid(1,1,2,2));
 assert(!pw_d3d9_texture_layout_valid(INT32_MIN,UINT32_MAX,4,4));assert(!pw_d3d9_texture_layout_valid(64,3,48,177));
 assert(!pw_d3d9_texture_layout_valid(64,0,48,48));assert(!pw_d3d9_texture_layout_valid(64,3,0,128));
 puts("d3d9 texture wire: PASS");return 0;
}
