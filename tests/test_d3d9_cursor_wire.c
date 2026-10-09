/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_cursor_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
 struct pw_d3d9_cursor_request q={10,7,9,0,1,2},d,old;struct pw_d3d9_cursor_reply r={10,0x1234,0},rr;unsigned char b[33];size_t n,i;
 assert(!pw_d3d9_cursor_encode(b,sizeof(b),&n,&q)&&n==32);assert(!pw_d3d9_cursor_decode(&d,b,n)&&!memcmp(&q,&d,sizeof(q)));
 old=d;b[28]=1;assert(pw_d3d9_cursor_decode(&d,b,n)&&!memcmp(&old,&d,sizeof(d)));b[28]=0;
 for(i=0;i<32;i++)assert(pw_d3d9_cursor_decode(&d,b,i));
 assert(pw_d3d9_cursor_decode(&d,b,33));
 q.generation=0;assert(pw_d3d9_cursor_encode(b,sizeof(b),&n,&q));q.id=0;assert(!pw_d3d9_cursor_encode(b,sizeof(b),&n,&q));
 q=(struct pw_d3d9_cursor_request){11,0x80000000,0xffffffff,0x80000001,0,0};assert(!pw_d3d9_cursor_encode(b,sizeof(b),&n,&q)&&!pw_d3d9_cursor_decode(&d,b,n)&&!memcmp(&q,&d,sizeof(q)));
 q=(struct pw_d3d9_cursor_request){12,0xffffffff,0,0,0,0};assert(!pw_d3d9_cursor_encode(b,sizeof(b),&n,&q));q.flags=1;assert(pw_d3d9_cursor_encode(b,sizeof(b),&n,&q));
 assert(!pw_d3d9_cursor_reply_encode(b,sizeof(b),&n,&r)&&!pw_d3d9_cursor_reply_decode(&rr,b,n)&&rr.hresult==0x1234);
 r=(struct pw_d3d9_cursor_reply){12,0,0xffffffff};assert(!pw_d3d9_cursor_reply_encode(b,sizeof(b),&n,&r)&&!pw_d3d9_cursor_reply_decode(&rr,b,n)&&rr.value==0xffffffff);
 r.hresult=0x80004005;assert(pw_d3d9_cursor_reply_encode(b,sizeof(b),&n,&r));r.value=0;assert(!pw_d3d9_cursor_reply_encode(b,sizeof(b),&n,&r));
 r=(struct pw_d3d9_cursor_reply){11,0,1};assert(pw_d3d9_cursor_reply_encode(b,sizeof(b),&n,&r));r.value=0;r.hresult=1;assert(pw_d3d9_cursor_reply_encode(b,sizeof(b),&n,&r));
 puts("PASS cursor wire: exact signed/BOOL bits, typed refs, HRESULT, canonical fields and atomic rejection");return 0;
}
