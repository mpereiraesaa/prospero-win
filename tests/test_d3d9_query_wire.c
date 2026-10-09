/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_query_wire.h"
#include <assert.h>
#include <string.h>
int main(void)
{
 unsigned char bytes[PW_D3D9_QUERY_WIRE_MAX+1];size_t n;
 struct pw_d3d9_query_request q={0},decoded;struct pw_d3d9_query_reply r={0},reply;
 for(unsigned type=0;type<20;type++)assert(pw_d3d9_query_type_size(type)==(type==4?16:type==8||type==9||type==11?4:type==10||type==12?8:0));
 q.method=PW_D3D9_QUERY_CREATE;q.type=8;q.want_object=1;
 assert(!pw_d3d9_query_request_encode(bytes,sizeof(bytes),&n,&q)&&n==32);
 for(size_t i=0;i<n;i++)assert(pw_d3d9_query_request_decode(&decoded,bytes,i));
 assert(!pw_d3d9_query_request_decode(&decoded,bytes,n));assert(pw_d3d9_query_request_decode(&decoded,bytes,n+1));
 r.method=q.method;r.object=(struct pw_d3d9_object_ref){3,9};r.type=8;r.size=4;
 assert(!pw_d3d9_query_reply_encode(bytes,sizeof(bytes),&n,&q,&r));assert(!pw_d3d9_query_reply_decode(&reply,&q,bytes,n));
 r.size=8;assert(pw_d3d9_query_reply_encode(bytes,sizeof(bytes),&n,&q,&r));
 memset(&q,0,sizeof(q));q.method=PW_D3D9_QUERY_DATA;q.flags=0xffffffffu;q.has_data=1;q.size=16;memset(q.data,0xa5,16);
 assert(!pw_d3d9_query_request_encode(bytes,sizeof(bytes),&n,&q)&&n==48);assert(!pw_d3d9_query_request_decode(&decoded,bytes,n)&&!memcmp(q.data,decoded.data,16));
 memset(&r,0,sizeof(r));r.method=q.method;r.count=16;memset(r.data,0xa5,16);
 for(unsigned hr=0;hr<3;hr++){r.hresult=hr==2?0x8876086cu:hr;assert(!pw_d3d9_query_reply_encode(bytes,sizeof(bytes),&n,&q,&r));assert(!pw_d3d9_query_reply_decode(&reply,&q,bytes,n)&&reply.hresult==r.hresult);for(size_t i=0;i<n;i++)assert(pw_d3d9_query_reply_decode(&reply,&q,bytes,i));}
 r.count=15;assert(pw_d3d9_query_reply_encode(bytes,sizeof(bytes),&n,&q,&r));q.size=17;assert(pw_d3d9_query_request_encode(bytes,sizeof(bytes),&n,&q));
 q.size=0;assert(!pw_d3d9_query_request_encode(bytes,sizeof(bytes),&n,&q)&&n==32);q.has_data=0;q.size=8;assert(!pw_d3d9_query_request_encode(bytes,sizeof(bytes),&n,&q)&&n==32);
 return 0;
}
