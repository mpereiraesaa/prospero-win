/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_object_getter.h"
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24;}
#define ROW(slot,name,args,kind,extra) {slot,args,kind,extra},
static const struct pw_d3d9_object_getter_schema schemas[]={PW_D3D9_OBJECT_GETTERS(ROW)};
#undef ROW
const struct pw_d3d9_object_getter_schema *pw_d3d9_object_getter_schema(uint32_t method)
{size_t i;for(i=0;i<sizeof(schemas)/sizeof(*schemas);i++)if(schemas[i].method==method)return schemas+i;return 0;}
static int valid(const struct pw_d3d9_object_getter_request *q)
{
 const struct pw_d3d9_object_getter_schema *s;unsigned i;
 if(!q)return PW_D3D9_OBJECT_GETTER_INVALID;
 if(!(s=pw_d3d9_object_getter_schema(q->method)))return PW_D3D9_OBJECT_GETTER_UNSUPPORTED;
 for(i=s->args;i<3;i++)if(q->args[i])return PW_D3D9_OBJECT_GETTER_INVALID;
 return 0;
}
int pw_d3d9_object_getter_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_object_getter_request *q)
{
 unsigned char *p=out;unsigned i;int r;if(!p||!written)return PW_D3D9_OBJECT_GETTER_INVALID;
 if((r=valid(q)))return r;
 if(cap<24)return PW_D3D9_OBJECT_GETTER_SMALL;
 put(p,1);put(p+4,q->method);for(i=0;i<3;i++)put(p+8+4*i,q->args[i]);put(p+20,0);*written=24;return 0;
}
int pw_d3d9_object_getter_decode(struct pw_d3d9_object_getter_request *out,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_object_getter_request q;unsigned i;int r;
 if(!out||!p||n!=24||get(p)!=1||get(p+20))return PW_D3D9_OBJECT_GETTER_INVALID;
 q.method=get(p+4);for(i=0;i<3;i++)q.args[i]=get(p+8+4*i);
 if((r=valid(&q)))return r;
 *out=q;return 0;
}
static int reply_valid(const struct pw_d3d9_object_getter_request *q,const struct pw_d3d9_object_getter_reply *r)
{
 const struct pw_d3d9_object_getter_schema *s;int status;
 if(!r)return PW_D3D9_OBJECT_GETTER_INVALID;
 if((status=valid(q)))return status;
 s=pw_d3d9_object_getter_schema(q->method);
 if(r->method!=q->method)return PW_D3D9_OBJECT_GETTER_INVALID;
 if(r->hresult&0x80000000u)return (r->kind||r->id||r->generation||r->offset||r->stride)?PW_D3D9_OBJECT_GETTER_INVALID:0;
 if((!r->id&&(r->kind||r->generation))||(r->id&&(!r->generation||r->kind!=s->kind)))return PW_D3D9_OBJECT_GETTER_INVALID;
 if(!s->extra&&(r->offset||r->stride))return PW_D3D9_OBJECT_GETTER_INVALID;
 return 0;
}
int pw_d3d9_object_getter_reply_encode(void *out,size_t cap,size_t *written,const struct pw_d3d9_object_getter_request *q,const struct pw_d3d9_object_getter_reply *r)
{
 unsigned char *p=out;int status;if(!p||!written)return PW_D3D9_OBJECT_GETTER_INVALID;
 if((status=reply_valid(q,r)))return status;
 if(cap<32)return PW_D3D9_OBJECT_GETTER_SMALL;
 put(p,1);put(p+4,r->method);put(p+8,r->hresult);put(p+12,r->kind);put(p+16,r->id);put(p+20,r->generation);put(p+24,r->offset);put(p+28,r->stride);*written=32;return 0;
}
int pw_d3d9_object_getter_reply_decode(struct pw_d3d9_object_getter_reply *out,const struct pw_d3d9_object_getter_request *q,const void *data,size_t n)
{
 const unsigned char *p=data;struct pw_d3d9_object_getter_reply r;int status;
 if(!p||!out||n!=32||get(p)!=1)return PW_D3D9_OBJECT_GETTER_INVALID;
 r.method=get(p+4);r.hresult=get(p+8);r.kind=get(p+12);r.id=get(p+16);r.generation=get(p+20);r.offset=get(p+24);r.stride=get(p+28);
 if((status=reply_valid(q,&r)))return status;
 *out=r;return 0;
}
