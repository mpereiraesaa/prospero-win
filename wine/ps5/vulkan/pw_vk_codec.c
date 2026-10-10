/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_codec.h"
#include <string.h>
#include <limits.h>
static int room(struct pw_vk_codec *c,size_t n)
{
 return c->used<=c->capacity && n<=c->capacity-c->used;
}
int pw_vk_codec_source(struct pw_vk_codec *c,const void *value,size_t bytes)
{
 uintptr_t source=(uintptr_t)value,target=(uintptr_t)c->wire;
 if(source>UINTPTR_MAX-bytes||target>UINTPTR_MAX-c->capacity)return 0;
 return !(source<target+c->capacity&&target<source+bytes);
}

int pw_vk_codec_value(struct pw_vk_codec *c,void *value,size_t n,unsigned wide)
{
 uint64_t v=0;size_t encoded=wide?8:n;
 if(!n||n>8||!room(c,encoded))return 0;
 if(!c->decode&&!pw_vk_codec_source(c,value,n))return 0;
 if(c->decode){
  if(wide){memcpy(&v,c->wire+c->used,8);if(n<8 && v>((UINT64_C(1)<<(n*8))-1))return 0;memcpy(value,&v,n);}
  else memcpy(value,c->wire+c->used,n);
 }else{
  if(wide){memcpy(&v,value,n);memcpy(c->wire+c->used,&v,8);}
  else memcpy(c->wire+c->used,value,n);
 }
 c->used+=encoded;return 1;
}
/* Contiguous same-width primitive elements retain scalar wire representation.
 * Widened pointer-size values and records still use the fieldwise path. */
int pw_vk_codec_bytes(struct pw_vk_codec *c,void *value,uint64_t count,size_t element)
{
 size_t bytes;
 if(count>UINT32_MAX||!element||element>8||count>SIZE_MAX/element)return 0;
 bytes=(size_t)count*element;
 if(!room(c,bytes))return 0;
 if(!bytes)return 1;
 if(!c->decode&&!pw_vk_codec_source(c,value,bytes))return 0;
 if(c->decode)memcpy(value,c->wire+c->used,bytes);
 else memcpy(c->wire+c->used,value,bytes);
 c->used+=bytes;return 1;
}
int pw_vk_codec_array(struct pw_vk_codec *c,void *slot,uint64_t count,size_t element,unsigned optional)
{
 void *pointer=NULL;uint32_t present=0;size_t bytes,offset;
 if(count>UINT32_MAX||!element||count>SIZE_MAX/element)return -1;
 bytes=(size_t)count*element;
 if(!c->decode){
  if(!pw_vk_codec_source(c,slot,sizeof(pointer)))return -1;
  memcpy(&pointer,slot,sizeof(pointer));present=pointer!=NULL;
  if(pointer&&!pw_vk_codec_source(c,pointer,bytes))return -1;
 }
 if(!pw_vk_codec_value(c,&present,sizeof(present),0)||present>1)return -1;
 if(!present){
  if(count&&!optional)return -1;
  if(c->decode)memcpy(slot,&pointer,sizeof(pointer));
  return 0;
 }
 if(bytes>(c->decode?c->arena_capacity:c->capacity))return -1;
 if(c->decode){
  if(c->arena_used>SIZE_MAX-7)return -1;
  offset=(c->arena_used+7)&~(size_t)7;
  if(offset>c->arena_capacity||bytes>c->arena_capacity-offset)return -1;
  pointer=c->arena+offset;c->arena_used=offset+bytes;
  memset(pointer,0,bytes);memcpy(slot,&pointer,sizeof(pointer));
 }
 return 1;
}
int pw_vk_codec_string(struct pw_vk_codec *c,void *slot)
{
 const char *string=NULL;uint32_t length=0,i;int present;
 if(!c->decode){
  memcpy(&string,slot,sizeof(string));
  if(string){for(;length<c->capacity;length++){if((uintptr_t)string>UINTPTR_MAX-length)return 0;if(!string[length]){length++;break;}}if(!length||length>c->capacity||string[length-1])return 0;}
 }
 if(!c->decode&&string&&!pw_vk_codec_source(c,string,length))return 0;
 if(!pw_vk_codec_value(c,&length,sizeof(length),0))return 0;
 present=pw_vk_codec_array(c,slot,length,1,1);if(present<0)return 0;
 if(!present)return length==0;
 memcpy(&string,slot,sizeof(string));
 for(i=0;i<length;i++)if(!pw_vk_codec_value(c,(void *)(string+i),1,0))return 0;
 return length&&string[length-1]==0;
}

/* Template snapshots have the existing normalized 32-byte header, followed by
 * sparse referenced spans. Only metadata-aware PE code can produce one. */
int pw_vk_codec_template(struct pw_vk_codec *c,void *slot,uint64_t handle)
{
 uint32_t bytes=0,span,reserved;uint64_t encoded_handle;size_t written,offset;void *pointer=NULL;
 if(!c->decode){
  if(!c->template_snapshot||!room(c,4)||!pw_vk_codec_source(c,slot,sizeof(pointer)))return 0;
  memcpy(&pointer,slot,sizeof(pointer));
  if(!c->template_snapshot(c->snapshot_context,handle,pointer,c->wire+c->used+4,c->capacity-c->used-4,&written)||written>UINT32_MAX)return 0;
  bytes=(uint32_t)written;
 }
 if(!pw_vk_codec_value(c,&bytes,4,0)||bytes<32||!room(c,bytes))return 0;
 memcpy(&reserved,c->wire+c->used+4,4);if(reserved)return 0;
 memcpy(&reserved,c->wire+c->used+28,4);if(reserved)return 0;
 memcpy(&encoded_handle,c->wire+c->used+16,8);if(encoded_handle!=handle)return 0;
 memcpy(&span,c->wire+c->used+24,4);if((uint64_t)span+32!=bytes)return 0;
 if(c->decode){
  if(c->arena_used>SIZE_MAX-7)return 0;
  offset=(c->arena_used+7)&~(size_t)7;
  if(offset>c->arena_capacity||span>c->arena_capacity-offset)return 0;
  pointer=c->arena+offset;c->arena_used=offset+span;
  memcpy(pointer,c->wire+c->used+32,span);memcpy(slot,&pointer,sizeof(pointer));
 }
 c->used+=bytes;return 1;
}
