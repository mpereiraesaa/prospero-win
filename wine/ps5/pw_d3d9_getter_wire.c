/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_getter_wire.h"
#include <string.h>
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
#define ROW(slot,name,args,bytes,element) {slot,args,bytes,element},
static const struct pw_d3d9_getter_schema schemas[]={PW_D3D9_GETTER_METHODS(ROW)};
#undef ROW
const struct pw_d3d9_getter_schema *pw_d3d9_getter_schema(uint32_t method)
{size_t i;for(i=0;i<sizeof(schemas)/sizeof(*schemas);i++)if(schemas[i].method==method)return schemas+i;return NULL;}
int pw_d3d9_getter_bytes(const struct pw_d3d9_getter_request *q,size_t *out)
{
 const struct pw_d3d9_getter_schema *s;uint64_t n;unsigned i;
 if(!q||!out)return PW_D3D9_GETTER_INVALID;
 if(!(s=pw_d3d9_getter_schema(q->method)))return PW_D3D9_GETTER_UNSUPPORTED;
 for(i=s->args;i<2;i++)if(q->args[i])return PW_D3D9_GETTER_INVALID;
 n=s->element?(uint64_t)s->element*q->args[1]:s->bytes;
 if(n>PW_D3D9_GETTER_DATA)return PW_D3D9_GETTER_UNSUPPORTED;
 *out=(size_t)n;return 0;
}
int pw_d3d9_getter_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_getter_request *q)
{
 unsigned char *p=out;size_t n;int r;
 if(!p||!written)return PW_D3D9_GETTER_INVALID;
 if((r=pw_d3d9_getter_bytes(q,&n)))return r;
 if(cap<24)return PW_D3D9_GETTER_SMALL;
 put(p,1);put(p+4,q->method);put(p+8,q->args[0]);put(p+12,q->args[1]);put(p+16,(uint32_t)n);put(p+20,0);*written=24;return 0;
}
int pw_d3d9_getter_decode(struct pw_d3d9_getter_request *out,const void *data,size_t bytes)
{
 const unsigned char *p=data;struct pw_d3d9_getter_request q;size_t n;int r;
 if(!out||!p||bytes!=24||get(p)!=1||get(p+20))return PW_D3D9_GETTER_INVALID;
 q.method=get(p+4);q.args[0]=get(p+8);q.args[1]=get(p+12);
 if((r=pw_d3d9_getter_bytes(&q,&n)))return r;
 if(n!=get(p+16))return PW_D3D9_GETTER_INVALID;
 *out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_getter_request *q,const struct pw_d3d9_getter_reply *r)
{
 size_t n;int status;if(!r)return PW_D3D9_GETTER_INVALID;
 if((status=pw_d3d9_getter_bytes(q,&n)))return status;
 if(r->method!=q->method||r->bytes!=((r->hresult&0x80000000u)?0:n))return PW_D3D9_GETTER_INVALID;
 return 0;
}
int pw_d3d9_getter_reply_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_getter_request *q,const struct pw_d3d9_getter_reply *r)
{
 unsigned char *p=out;size_t i,n;int status;if(!p||!written)return PW_D3D9_GETTER_INVALID;
 if((status=reply_valid(q,r)))return status;
 n=16+r->bytes;if(cap<n)return PW_D3D9_GETTER_SMALL;
 put(p,1);put(p+4,r->method);put(p+8,r->hresult);put(p+12,r->bytes);
 if(r->method==72)memcpy(p+16,r->data.bytes,r->bytes);
 else for(i=0;i<r->bytes/4;i++)put(p+16+4*i,r->data.words[i]);
 *written=n;return 0;
}
int pw_d3d9_getter_reply_decode(struct pw_d3d9_getter_reply *out,const struct pw_d3d9_getter_request *q,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_getter_reply r={0};size_t i;int status;
 if(!out||!p||n<16||n>PW_D3D9_GETTER_MAX||get(p)!=1)return PW_D3D9_GETTER_INVALID;
 r.method=get(p+4);r.hresult=get(p+8);r.bytes=get(p+12);
 if((status=reply_valid(q,&r)))return status;
 if(n!=16+(size_t)r.bytes)return PW_D3D9_GETTER_INVALID;
 if(r.method==72)memcpy(r.data.bytes,p+16,r.bytes);
 else for(i=0;i<r.bytes/4;i++)r.data.words[i]=get(p+16+4*i);
 *out=r;return 0;
}
