/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_resource_wire.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void request_roundtrip(unsigned op)
{
 struct pw_d3d9_resource_request q={0},r,old;
 unsigned char wire[PW_D3D9_RESOURCE_MAX_WIRE],copy[sizeof(wire)];size_t n,i;
 q.operation=op;q.length=0x12345678;q.usage=0x87654321;q.format_fvf=101;q.pool=2;
 q.offset=11;q.flags=0x3010;q.count=PW_D3D9_RESOURCE_CHUNK;q.lock_generation=UINT64_C(0x1122334455667788);
 for(i=0;i<q.count;i++)q.data[i]=(unsigned char)i;
 assert(!pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,&q));
 assert(wire[0]==1&&wire[4]==op&&wire[12]==0);
 assert(!pw_d3d9_resource_request_decode(&r,wire,n));assert(r.operation==op);
 assert(!pw_d3d9_resource_request_encode(copy,sizeof(copy),&i,&r));assert(i==n&&!memcmp(copy,wire,n));
 memset(&r,0x55,sizeof(r));old=r;
 for(i=0;i<n;i++){assert(pw_d3d9_resource_request_decode(&r,wire,i));assert(!memcmp(&r,&old,sizeof(r)));}
 assert(pw_d3d9_resource_request_decode(&r,wire,n+1));
 wire[12]=1;assert(pw_d3d9_resource_request_decode(&r,wire,n));wire[12]=0;
 memset(copy,0xaa,sizeof(copy));assert(pw_d3d9_resource_request_encode(copy,n-1,&i,&q)==PW_D3D9_RESOURCE_SMALL);assert(i==0&&copy[0]==0xaa);
}
static void reply_roundtrip(unsigned op,uint32_t hr)
{
 struct pw_d3d9_resource_reply r={0},decoded,old;size_t n,i;
 unsigned char wire[PW_D3D9_RESOURCE_MAX_WIRE],copy[sizeof(wire)];
 r.operation=op;r.hresult=hr;r.object=(struct pw_d3d9_object_ref){7,9};
 r.desc=(struct pw_d3d9_buffer_desc){101,6,0x200,1,1000,0x112};r.length=1000;
 r.lock_generation=UINT64_C(0xfedcba9876543210);r.offset=8;r.count=4096;
 for(i=0;i<r.count;i++)r.data[i]=(unsigned char)(i+11);
 assert(!pw_d3d9_resource_reply_encode(wire,sizeof(wire),&n,&r));
 assert(!pw_d3d9_resource_reply_decode(&decoded,wire,n));
 assert(decoded.operation==op&&decoded.hresult==hr);
 if(hr&0x80000000u)assert(n==16&&!decoded.object.id&&!decoded.count&&!decoded.lock_generation);
 assert(!pw_d3d9_resource_reply_encode(copy,sizeof(copy),&i,&decoded));assert(i==n&&!memcmp(copy,wire,n));
 memset(&decoded,0x55,sizeof(decoded));old=decoded;
 for(i=0;i<n;i++){assert(pw_d3d9_resource_reply_decode(&decoded,wire,i));assert(!memcmp(&decoded,&old,sizeof(old)));}
 assert(pw_d3d9_resource_reply_decode(&decoded,wire,n+1));
}
int main(void)
{
 unsigned op;size_t n;unsigned char wire[PW_D3D9_RESOURCE_MAX_WIRE];
 struct pw_d3d9_resource_request q={.operation=PW_D3D9_RESOURCE_WRITE,.lock_generation=1,.count=1};
 struct pw_d3d9_resource_reply r={.operation=PW_D3D9_RESOURCE_LOCK,.lock_generation=1,.length=1};
 for(op=1;op<=8;op++){request_roundtrip(op);reply_roundtrip(op,0);reply_roundtrip(op,0x8876086a);}
 q.offset=UINT32_MAX;assert(pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,&q));
 q.offset=0;q.count=4097;assert(pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,&q));
 q.count=0;assert(pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,&q));
 q.count=1;q.lock_generation=0;assert(pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,&q));
 q.operation=999;assert(pw_d3d9_resource_request_encode(wire,sizeof(wire),&n,&q)==PW_D3D9_RESOURCE_UNSUPPORTED);
 r.length=PW_D3D9_RESOURCE_MAX_LOCK+1;assert(pw_d3d9_resource_reply_encode(wire,sizeof(wire),&n,&r));
 r.length=1;r.lock_generation=0;assert(pw_d3d9_resource_reply_encode(wire,sizeof(wire),&n,&r));
 puts("d3d9 resource wire: PASS");return 0;
}
