/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_cursor_wire.h"
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
static int valid(const struct pw_d3d9_cursor_request *q)
{
 if(!q)return 0;
 if(q->method==10)return !q->flags&&!!q->id==!!q->generation;
 if(q->method==11)return !q->id&&!q->generation;
 return q->method==12&&!q->arg1&&!q->flags&&!q->id&&!q->generation;
}
int pw_d3d9_cursor_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_cursor_request *q)
{
 unsigned char *p=out;if(!p||!written||!valid(q))return 1;if(cap<32)return 2;
 put(p,1);put(p+4,q->method);put(p+8,q->arg0);put(p+12,q->arg1);put(p+16,q->flags);put(p+20,q->id);put(p+24,q->generation);put(p+28,0);*written=32;return 0;
}
int pw_d3d9_cursor_decode(struct pw_d3d9_cursor_request *out,const void *data,size_t size)
{
 const unsigned char *p=data;struct pw_d3d9_cursor_request q;
 if(!out||!p||size!=32||get(p)!=1||get(p+28))return 1;
 q=(struct pw_d3d9_cursor_request){get(p+4),get(p+8),get(p+12),get(p+16),get(p+20),get(p+24)};
 if(!valid(&q))return 1;
 *out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_cursor_reply *r)
{
 if(!r||r->method<10||r->method>12)return 0;
 if(r->hresult&0x80000000u)return !r->value;
 if(r->method==10)return !r->value;
 return !r->hresult&&(r->method==12||!r->value);
}
int pw_d3d9_cursor_reply_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_cursor_reply *r)
{
 unsigned char *p=out;if(!p||!written||!reply_valid(r))return 1;if(cap<16)return 2;
 put(p,1);put(p+4,r->method);put(p+8,r->hresult);put(p+12,r->value);*written=16;return 0;
}
int pw_d3d9_cursor_reply_decode(struct pw_d3d9_cursor_reply *out,const void *data,size_t size)
{
 const unsigned char *p=data;struct pw_d3d9_cursor_reply r;
 if(!out||!p||size!=16||get(p)!=1)return 1;
 r=(struct pw_d3d9_cursor_reply){get(p+4),get(p+8),get(p+12)};
 if(!reply_valid(&r))return 1;
 *out=r;return 0;
}
