/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_up_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
 struct pw_d3d9_up_request q={0},decoded,old;struct pw_d3d9_up_draw d;
 struct pw_d3d9_up_upload u;struct pw_d3d9_up_reply r={1,0,1},rr;
 unsigned char wire[PW_D3D9_UP_WIRE_MAX],storage[1024],expected[1024];size_t n;uint64_t token,next;unsigned i;
 q.operation=1;q.draw=(struct pw_d3d9_up_draw){83,4,0,0,1,20,0,0,0};
 assert(!pw_d3d9_up_measure(&q.draw)&&q.draw.vertex_bytes==60&&!q.draw.index_bytes);
 for(i=1;i<=6;i++){static const unsigned counts[]={0,2,4,3,6,4,4};d=q.draw;d.primitive_type=i;d.primitive_count=2;assert(!pw_d3d9_up_measure(&d)&&d.vertex_bytes==counts[i]*20);}
 d=q.draw;d.primitive_count=UINT32_MAX;assert(pw_d3d9_up_measure(&d));assert(d.vertex_bytes==60);
 d=q.draw;d.stride=UINT32_MAX;assert(pw_d3d9_up_measure(&d));
 d=q.draw;d.method=84;d.min_vertex=2;d.num_vertices=3;d.index_format=101;assert(!pw_d3d9_up_measure(&d)&&d.vertex_bytes==100&&d.index_bytes==6);
 d.index_format=102;assert(!pw_d3d9_up_measure(&d)&&d.index_bytes==12);
 d.min_vertex=UINT32_MAX;assert(pw_d3d9_up_measure(&d));
 d=q.draw;d.primitive_count=0;d.primitive_type=UINT32_MAX;assert(!pw_d3d9_up_measure(&d)&&!d.vertex_bytes);
 d=q.draw;d.method=84;d.num_vertices=1;d.stride=PW_D3D9_UP_LIMIT;d.index_format=101;assert(pw_d3d9_up_measure(&d));
 assert(!pw_d3d9_up_encode(wire,sizeof(wire),&n,&q)&&n==64);
 memset(&decoded,0xcc,sizeof(decoded));old=decoded;wire[60]=1;assert(pw_d3d9_up_decode(&decoded,wire,n)&&!memcmp(&decoded,&old,sizeof(old)));wire[60]=0;
 assert(!pw_d3d9_up_decode(&decoded,wire,n)&&!memcmp(&decoded.draw,&q.draw,sizeof(q.draw)));
 for(i=0;i<n;i++)assert(pw_d3d9_up_decode(&decoded,wire,i));
 assert(pw_d3d9_up_decode(&decoded,wire,n+1));
 pw_d3d9_up_upload_init(&u,storage,sizeof(storage));assert(!pw_d3d9_up_upload_apply(&u,&q,&token)&&token==1);
 assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_BUSY);
 memset(&q,0,sizeof(q));q.operation=3;q.transfer=token;assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_INVALID);
 q.operation=2;q.count=60;for(i=0;i<60;i++)q.data[i]=expected[i]=(unsigned char)i;
 q.offset=1;assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_INVALID);q.offset=0;
 assert(!pw_d3d9_up_encode(wire,sizeof(wire),&n,&q)&&n==124);assert(!pw_d3d9_up_decode(&decoded,wire,n));
 assert(!pw_d3d9_up_upload_apply(&u,&decoded,&next)&&!memcmp(storage,expected,60));memset(decoded.data,0,60);assert(!memcmp(storage,expected,60));
 assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_INVALID);
 memset(&q,0,sizeof(q));q.operation=3;q.transfer=token;assert(!pw_d3d9_up_upload_apply(&u,&q,&next)&&u.ready);
 assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_BUSY);
 pw_d3d9_up_upload_finish(&u);assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_STALE);
 memset(&q,0,sizeof(q));q.operation=1;q.draw=(struct pw_d3d9_up_draw){83,0,0,0,0,0,0,0,0};assert(!pw_d3d9_up_upload_apply(&u,&q,&token)&&token==2);
 memset(&q,0,sizeof(q));q.operation=4;q.transfer=token;assert(!pw_d3d9_up_upload_apply(&u,&q,&next)&&!u.transfer);
 q.operation=1;q.transfer=0;q.draw.method=83;u.next=UINT64_MAX;assert(pw_d3d9_up_upload_apply(&u,&q,&next)==PW_D3D9_UP_EXHAUSTED);
 assert(!pw_d3d9_up_reply_encode(wire,sizeof(wire),&n,&r)&&n==24);assert(!pw_d3d9_up_reply_decode(&rr,wire,n)&&rr.transfer==1);
 r.hresult=0x8876086c;r.transfer=0;assert(!pw_d3d9_up_reply_encode(wire,sizeof(wire),&n,&r));assert(!pw_d3d9_up_reply_decode(&rr,wire,n)&&rr.hresult==r.hresult);
 r.transfer=1;assert(pw_d3d9_up_reply_encode(wire,sizeof(wire),&n,&r));
 puts("PASS UP codec: exact spans, bounded overflow, canonical wire, owned ordered upload, stale/duplicate/incomplete/exhausted rejection");return 0;
}
