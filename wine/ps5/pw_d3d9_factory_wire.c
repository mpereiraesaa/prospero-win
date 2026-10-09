/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_d3d9_factory_wire.h"
#include <string.h>
#define PW_CAP_COUNT(type,name,native) +1
_Static_assert(0 PW_D3D9_CAP_FIELDS(PW_CAP_COUNT) == PW_D3D9_CAP_WORDS,"caps field count");
#undef PW_CAP_COUNT
#define OFF(member) offsetof(struct pw_d3d9_factory_request,member)
struct request_schema { unsigned count; size_t offset[6]; };
static const struct request_schema schemas[15] = {
 [4]={0,{0}}, [5]={2,{OFF(adapter),OFF(flags)}},
 [6]={2,{OFF(adapter),OFF(format)}}, [7]={3,{OFF(adapter),OFF(format),OFF(mode)}},
 [8]={1,{OFF(adapter)}},
 [9]={5,{OFF(adapter),OFF(device_type),OFF(format),OFF(format2),OFF(windowed)}},
 [10]={6,{OFF(adapter),OFF(device_type),OFF(format),OFF(usage),OFF(resource_type),OFF(format2)}},
 [11]={5,{OFF(adapter),OFF(device_type),OFF(format),OFF(windowed),OFF(multisample)}},
 [12]={5,{OFF(adapter),OFF(device_type),OFF(format),OFF(format2),OFF(format3)}},
 [13]={4,{OFF(adapter),OFF(device_type),OFF(format),OFF(format2)}},
 [14]={2,{OFF(adapter),OFF(device_type)}}
};
#undef OFF
static uint32_t get32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(unsigned char *p,uint32_t n)
{unsigned i;for(i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static int method_status(uint32_t method)
{
 if(method==3 || method==15 || method==16)return PW_D3D9_FACTORY_UNSUPPORTED;
 return method>=4 && method<=14 ? PW_D3D9_FACTORY_OK : PW_D3D9_FACTORY_INVALID;
}
static int boolean_valid(const struct pw_d3d9_factory_request *r)
{return (r->method!=9 && r->method!=11) || r->windowed<=1;}
int pw_d3d9_factory_request_encode(void *wire,size_t capacity,size_t *written,
 const struct pw_d3d9_factory_request *r)
{
 unsigned char tmp[40]={0};const struct request_schema *s;unsigned i;int status;
 if(!written || !r)return PW_D3D9_FACTORY_INVALID;
 *written=0;if((status=method_status(r->method)))return status;
 if(!boolean_valid(r))return PW_D3D9_FACTORY_INVALID;
 s=&schemas[r->method];*written=16+4*s->count;
 if(capacity<*written)return PW_D3D9_FACTORY_SMALL;
 if(!wire)return PW_D3D9_FACTORY_INVALID;
 put32(tmp,PW_D3D9_FACTORY_VERSION);put32(tmp+4,r->method);put32(tmp+8,s->count);
 for(i=0;i<s->count;i++)put32(tmp+16+4*i,*(const uint32_t *)((const unsigned char *)r+s->offset[i]));
 memcpy(wire,tmp,*written);return PW_D3D9_FACTORY_OK;
}
int pw_d3d9_factory_request_decode(struct pw_d3d9_factory_request *out,const void *wire,size_t bytes)
{
 const unsigned char *p=wire;struct pw_d3d9_factory_request r={0};const struct request_schema *s;
 unsigned i;int status;
 if(!out || !wire || bytes<16 || get32(p)!=PW_D3D9_FACTORY_VERSION || get32(p+12))return PW_D3D9_FACTORY_INVALID;
 r.method=get32(p+4);if((status=method_status(r.method)))return status;
 s=&schemas[r.method];if(get32(p+8)!=s->count || bytes!=16+4*s->count)return PW_D3D9_FACTORY_INVALID;
 for(i=0;i<s->count;i++)*(uint32_t *)((unsigned char *)&r+s->offset[i])=get32(p+16+4*i);
 if(!boolean_valid(&r))return PW_D3D9_FACTORY_INVALID;
 *out=r;return PW_D3D9_FACTORY_OK;
}
static unsigned output_bytes(uint32_t method,uint32_t hr)
{
 if(hr&UINT32_C(0x80000000))return 0;
 switch(method){
 case 4:case 6:case 11:return 4;
 case 5:return 1100;
 case 7:case 8:return 16;
 case 14:return PW_D3D9_CAP_WORDS*4;
 default:return 0;
 }
}
/* Write only through the first NUL; the temporary wire buffer is zeroed.
 * Decoding requires canonical zero tails, avoiding hidden uninitialized data. */
static int string_size(const char *p,size_t bytes,size_t *length)
{
 const char *end=memchr(p,0,bytes);if(!end)return 0;*length=(size_t)(end-p)+1;return 1;
}
static int encode_identifier(unsigned char *p,const struct pw_d3d9_adapter_identifier *id)
{
 size_t a,b,c;
 if(!string_size(id->driver,512,&a) || !string_size(id->description,512,&b) || !string_size(id->device_name,32,&c))return 0;
 memcpy(p,id->driver,a);memcpy(p+512,id->description,b);memcpy(p+1024,id->device_name,c);
 put32(p+1056,(uint32_t)id->driver_version);put32(p+1060,(uint32_t)(id->driver_version>>32));
 put32(p+1064,id->vendor_id);put32(p+1068,id->device_id);put32(p+1072,id->subsystem_id);put32(p+1076,id->revision);
 put32(p+1080,id->guid_data1);p[1084]=(unsigned char)id->guid_data2;p[1085]=(unsigned char)(id->guid_data2>>8);
 p[1086]=(unsigned char)id->guid_data3;p[1087]=(unsigned char)(id->guid_data3>>8);
 memcpy(p+1088,id->guid_data4,8);put32(p+1096,id->whql_level);return 1;
}
static int canonical_string(const unsigned char *p,size_t bytes)
{
 size_t n,i;if(!string_size((const char *)p,bytes,&n))return 0;
 for(i=n;i<bytes;i++)if(p[i])return 0;
 return 1;
}
static int decode_identifier(struct pw_d3d9_adapter_identifier *id,const unsigned char *p)
{
 if(!canonical_string(p,512) || !canonical_string(p+512,512) || !canonical_string(p+1024,32))return 0;
 memcpy(id->driver,p,512);memcpy(id->description,p+512,512);memcpy(id->device_name,p+1024,32);
 id->driver_version=get32(p+1056)|(uint64_t)get32(p+1060)<<32;
 id->vendor_id=get32(p+1064);id->device_id=get32(p+1068);id->subsystem_id=get32(p+1072);id->revision=get32(p+1076);
 id->guid_data1=get32(p+1080);id->guid_data2=(uint16_t)(p[1084]|(unsigned)p[1085]<<8);
 id->guid_data3=(uint16_t)(p[1086]|(unsigned)p[1087]<<8);memcpy(id->guid_data4,p+1088,8);
 id->whql_level=get32(p+1096);return 1;
}
int pw_d3d9_factory_reply_encode(void *wire,size_t capacity,size_t *written,const struct pw_d3d9_factory_reply *r)
{
 unsigned char tmp[PW_D3D9_FACTORY_MAX_REPLY]={0},*p=tmp+16;unsigned bytes,i=0;int status;
 if(!written || !r)return PW_D3D9_FACTORY_INVALID;
 *written=0;if((status=method_status(r->method)))return status;
 if((r->method==4 || r->method==6) && r->hresult)return PW_D3D9_FACTORY_INVALID;
 bytes=output_bytes(r->method,r->hresult);
 if(bytes)switch(r->method){
 case 4:case 6:case 11:put32(p,r->count);break;
 case 5:if(!encode_identifier(p,&r->identifier))return PW_D3D9_FACTORY_INVALID;break;
 case 7:case 8:put32(p,r->mode.width);put32(p+4,r->mode.height);put32(p+8,r->mode.refresh_rate);put32(p+12,r->mode.format);break;
 case 14:
#define PW_CAP_ENCODE(type,name,native) put32(p+4*i++,r->caps.name);
 PW_D3D9_CAP_FIELDS(PW_CAP_ENCODE)
#undef PW_CAP_ENCODE
 break;
 }
 *written=16+bytes;if(capacity<*written)return PW_D3D9_FACTORY_SMALL;
 if(!wire)return PW_D3D9_FACTORY_INVALID;
 put32(tmp,PW_D3D9_FACTORY_VERSION);put32(tmp+4,r->method);put32(tmp+8,r->hresult);put32(tmp+12,bytes);
 memcpy(wire,tmp,*written);return PW_D3D9_FACTORY_OK;
}
int pw_d3d9_factory_reply_decode(struct pw_d3d9_factory_reply *out,const void *wire,size_t bytes)
{
 const unsigned char *p=wire;struct pw_d3d9_factory_reply r={0};unsigned count,i=0;int status;
 if(!out || !wire || bytes<16 || get32(p)!=PW_D3D9_FACTORY_VERSION)return PW_D3D9_FACTORY_INVALID;
 r.method=get32(p+4);r.hresult=get32(p+8);if((status=method_status(r.method)))return status;
 if((r.method==4 || r.method==6) && r.hresult)return PW_D3D9_FACTORY_INVALID;
 count=output_bytes(r.method,r.hresult);if(get32(p+12)!=count || bytes!=16+count)return PW_D3D9_FACTORY_INVALID;
 p+=16;if(count)switch(r.method){
 case 4:case 6:case 11:r.count=get32(p);break;
 case 5:if(!decode_identifier(&r.identifier,p))return PW_D3D9_FACTORY_INVALID;break;
 case 7:case 8:r.mode.width=get32(p);r.mode.height=get32(p+4);r.mode.refresh_rate=get32(p+8);r.mode.format=get32(p+12);break;
 case 14:
#define PW_CAP_DECODE(type,name,native) r.caps.name=get32(p+4*i++);
 PW_D3D9_CAP_FIELDS(PW_CAP_DECODE)
#undef PW_CAP_DECODE
 break;
 }
 *out=r;return PW_D3D9_FACTORY_OK;
}
