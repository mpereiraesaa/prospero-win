/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_query_wire.h"
#include <string.h>
static void put(uint8_t *p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static uint32_t get(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
uint32_t pw_d3d9_query_type_size(uint32_t type)
{switch(type){case 4:return 16;case 8:case 9:case 11:return 4;case 10:case 12:return 8;default:return 0;}}
static int valid(const struct pw_d3d9_query_request *q)
{
 if(!q || q->has_data>1 || q->want_object>1 || q->size>PW_D3D9_QUERY_DATA_MAX)return 0;
 switch(q->method){
 case PW_D3D9_QUERY_CREATE:return !q->flags && !q->size && !q->has_data;
 case PW_D3D9_QUERY_ISSUE:return !q->type && !q->size && !q->has_data && !q->want_object;
 case PW_D3D9_QUERY_DATA:return !q->type && !q->want_object;
 default:return 0;
 }
}
int pw_d3d9_query_request_encode(void *out,size_t capacity,size_t *length,const struct pw_d3d9_query_request *q)
{
 uint8_t *p=out;size_t count,n;if(!p || !length || !valid(q))return -1;
 count=q->has_data?q->size:0;n=32+count;if(capacity<n)return -1;
 memset(p,0,n);put(p,1);put(p+4,q->method);put(p+8,q->type);put(p+12,q->flags);put(p+16,q->size);put(p+20,q->has_data);put(p+24,q->want_object);memcpy(p+32,q->data,count);*length=n;return 0;
}
int pw_d3d9_query_request_decode(struct pw_d3d9_query_request *q,const void *in,size_t length)
{
 const uint8_t *p=in;struct pw_d3d9_query_request t={0};size_t count;
 if(!q || !p || length<32 || get(p)!=1 || get(p+28))return -1;
 t.method=get(p+4);t.type=get(p+8);t.flags=get(p+12);t.size=get(p+16);t.has_data=get(p+20);t.want_object=get(p+24);
 if(!valid(&t))return -1;
 count=t.has_data?t.size:0;if(length!=32+count)return -1;memcpy(t.data,p+32,count);*q=t;return 0;
}
static int reply_valid(const struct pw_d3d9_query_request *q,const struct pw_d3d9_query_reply *r)
{
 if(!valid(q) || !r || r->method!=q->method || r->count>PW_D3D9_QUERY_DATA_MAX)return 0;
 if(q->method==PW_D3D9_QUERY_CREATE){
  if(r->count)return 0;
  if((r->hresult&0x80000000u) || !q->want_object)return !r->object.id && !r->object.generation && !r->type && !r->size;
  return r->object.id && r->object.generation && r->type==q->type && r->size && r->size==pw_d3d9_query_type_size(r->type);
 }
 if(r->object.id || r->object.generation || r->type || r->size)return 0;
 return r->count==(q->method==PW_D3D9_QUERY_DATA && q->has_data?q->size:0);
}
int pw_d3d9_query_reply_encode(void *out,size_t capacity,size_t *length,const struct pw_d3d9_query_request *q,const struct pw_d3d9_query_reply *r)
{
 uint8_t *p=out;size_t n;if(!p || !length || !reply_valid(q,r))return -1;n=32+r->count;if(capacity<n)return -1;
 put(p,1);put(p+4,r->method);put(p+8,r->hresult);put(p+12,r->object.id);put(p+16,r->object.generation);put(p+20,r->type);put(p+24,r->size);put(p+28,r->count);memcpy(p+32,r->data,r->count);*length=n;return 0;
}
int pw_d3d9_query_reply_decode(struct pw_d3d9_query_reply *r,const struct pw_d3d9_query_request *q,const void *in,size_t length)
{
 const uint8_t *p=in;struct pw_d3d9_query_reply t={0};if(!r || !p || length<32 || get(p)!=1)return -1;
 t.method=get(p+4);t.hresult=get(p+8);t.object.id=get(p+12);t.object.generation=get(p+16);t.type=get(p+20);t.size=get(p+24);t.count=get(p+28);
 if(!reply_valid(q,&t) || length!=32+t.count)return -1;
 memcpy(t.data,p+32,t.count);*r=t;return 0;
}
