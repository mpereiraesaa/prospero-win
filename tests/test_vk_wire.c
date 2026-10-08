/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_template_cache.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int fail;static void *allocate(size_t n){return fail?NULL:malloc(n);}
static int template_cache_cases(void){
 struct pw_vk_template_cache c={0};struct pw_vk_template_alloc a={allocate,free};
 struct pw_vk_template_entry e={6,1,0,24};const struct pw_vk_template_entry *p;size_t n;
 uint64_t h=UINT64_C(0x100000001);
 assert(pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));e.count=7;
 p=pw_vk_template_lookup(&c,1,h,&n);assert(p&&n==1&&p->count==1);
 assert(!pw_vk_template_lookup(&c,2,h,&n));assert(!pw_vk_template_lookup(&c,1,1,&n));
 assert(!pw_vk_template_register(&c,&a,1,h,0,0,0,0,&e,1));assert(pw_vk_template_lookup(&c,1,h,&n));
 assert(!pw_vk_template_register(&c,&a,1,h,1,1,0,0,&e,1));assert(!pw_vk_template_lookup(&c,1,h,&n));
 assert(!pw_vk_template_register(&c,&a,1,h,1,0,1,0,&e,1));
 assert(pw_vk_template_register(&c,&a,1,h,1,0,0,1,&e,1));
 assert(!pw_vk_template_register(&c,&a,1,h,1,0,0,2,&e,1));
 e.type=UINT32_MAX;assert(!pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));e.type=6;
 {
  struct pw_vk_template_entry inline_bytes={1000138000,7,3,UINT64_MAX};
  unsigned char data[10]={0,0,0,1,2,3,4,5,6,7},out[128];size_t bytes,extent;
  assert(pw_vk_template_extent(&inline_bytes,1,&extent)&&extent==10);
  assert(pw_vk_wire_template(out,sizeof(out),1,0,h,&inline_bytes,1,data,&bytes)&&bytes==42);
  assert(!memcmp(out+35,data+3,7));memset(data,0xcc,sizeof(data));assert(out[35]==1);
  struct pw_vk_template_entry accel={1000150000,2,0,8};uint64_t handles[2]={h,h+1};
  assert(pw_vk_wire_template(out,sizeof(out),1,0,h,&accel,1,handles,&bytes)&&bytes==48);
  assert(pw_vk_wire_u64(out+32)==h&&pw_vk_wire_u64(out+40)==h+1);
  accel.type=1000165000;assert(pw_vk_template_extent(&accel,1,&extent)&&extent==16);
  assert(pw_vk_template_register(&c,&a,1,h,1,0,0,1,NULL,0));
  assert(pw_vk_template_snapshot(&c,1,0,h,NULL,0,out,sizeof(out),&bytes)&&bytes==32);
  pw_vk_template_remove(&c,&a,1,h);
 }
 fail=1;assert(!pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));fail=0;
 assert(pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));assert(pw_vk_template_register(&c,&a,2,h,1,0,0,0,&e,1));
 pw_vk_template_remove_device(&c,&a,1);assert(!pw_vk_template_lookup(&c,1,h,&n));assert(pw_vk_template_lookup(&c,2,h,&n));
 pw_vk_template_remove(&c,&a,2,h);assert(!c.count);
 {
  struct pw_vk_template_entry sparse[]={ {6,2,8,40},{4,1,88,8} };
  unsigned char data[100],out[140];size_t bytes,extent;unsigned i;
  for(i=0;i<sizeof(data);i++)data[i]=(unsigned char)(i+1);
  assert(pw_vk_template_extent(sparse,2,&extent)&&extent==96);
  assert(pw_vk_template_register(&c,&a,3,h,1,0,0,0,sparse,2));
  assert(pw_vk_template_snapshot(&c,3,12,h,data,0,out,sizeof(out),&bytes)&&bytes==128);
  for(i=0;i<96;i++) {int used=(i>=8&&i<32)||(i>=48&&i<72)||(i>=88&&i<96);assert(out[32+i]==(used?data[i]:0));}
  for(i=0;i<100;i++)data[i]=0;
  assert(out[40]==9); /* caller reuse after return */
  assert(!pw_vk_template_snapshot(&c,3,12,h,data,0,out,127,&bytes));
  assert(!pw_vk_template_snapshot(&c,3,12,h,(void *)(UINTPTR_MAX-32),0,out,sizeof(out),&bytes));
  assert(!pw_vk_template_snapshot(&c,3,12,h,(void *)(uintptr_t)(UINT32_MAX-32),1,out,sizeof(out),&bytes));
  sparse[0].offset=UINT64_MAX;assert(!pw_vk_template_extent(sparse,2,&extent));
  sparse[0].offset=0;sparse[0].stride=UINT64_MAX;assert(!pw_vk_template_extent(sparse,2,&extent));
  sparse[0].stride=24;sparse[0].count=UINT32_MAX;assert(!pw_vk_template_extent(sparse,2,&extent));
  sparse[0].count=1;sparse[0].type=UINT32_MAX;assert(!pw_vk_template_extent(sparse,2,&extent));
  pw_vk_template_remove_device(&c,&a,3);assert(!c.count);
 }
 return 0;
}

#include <stdio.h>
#include <string.h>

/* Find real colliding keys without duplicating the hash implementation. */
static size_t occupied_bucket(const struct pw_vk_template_cache *c)
{
 size_t i;for(i=0;i<PW_VK_TEMPLATE_BUCKETS;i++)if(c->buckets[i])return i;
 assert(0);return 0;
}
static void hash_cache_cases(void)
{
 struct pw_vk_template_cache c={0};struct pw_vk_template_alloc a={allocate,free};
 struct pw_vk_template_entry e={6,1,0,24};size_t n,bucket=0,found=0;uint64_t keys[3],h;
 /* Three entries deliberately share a bucket; remove tail, middle and head
  * across rebuilds, preserving the other two and their exact identities. */
 for(h=1;found<3;h++) {
  assert(pw_vk_template_register(&c,&a,8,h,1,0,0,0,&e,1));
  size_t b=occupied_bucket(&c);
  if(!found){bucket=b;keys[found++]=h;}
  else if(b==bucket)keys[found++]=h;
  pw_vk_template_remove(&c,&a,8,h);
 }
 for(size_t victim=0;victim<3;victim++) {
  for(size_t i=0;i<3;i++)assert(pw_vk_template_register(&c,&a,8,keys[i],1,0,0,0,&e,1));
  assert(c.count==3);
  if(victim==1) {
   fail=1;assert(!pw_vk_template_register(&c,&a,8,keys[victim],1,0,0,0,&e,1));fail=0;
  } else pw_vk_template_remove(&c,&a,8,keys[victim]);
  for(size_t i=0;i<3;i++)assert(!!pw_vk_template_lookup(&c,8,keys[i],&n)==(i!=victim));
  pw_vk_template_remove_device(&c,&a,8);assert(!c.count);
 }
 /* More templates than buckets prove collision chains do not impose a hard
  * capacity. Distinct full-width handles and devices survive full lookup. */
 for(size_t i=0;i<PW_VK_TEMPLATE_BUCKETS*4u;i++) {
  e.count=(uint32_t)(i%16+1);h=UINT64_C(0x100000000)+(uint64_t)i;
  assert(pw_vk_template_register(&c,&a,(uint32_t)(3+i%2),h,1,0,0,0,&e,1));
 }
 assert(c.count==PW_VK_TEMPLATE_BUCKETS*4u);
 for(size_t i=0;i<PW_VK_TEMPLATE_BUCKETS*4u;i++) {
  h=UINT64_C(0x100000000)+(uint64_t)i;
  const struct pw_vk_template_entry *p=pw_vk_template_lookup(&c,(uint32_t)(3+i%2),h,&n);
  assert(p&&n==1&&p->count==i%16+1);
  assert(!pw_vk_template_lookup(&c,(uint32_t)(4-i%2),h,&n));
  assert(!pw_vk_template_lookup(&c,(uint32_t)(3+i%2),(uint32_t)h,&n));
 }
 /* Failed creation keeps prior metadata; successful reuse on OOM retires it.
  * Unrelated colliding templates must remain available. */
 h=UINT64_C(0x100000000)+2;
 fail=1;assert(!pw_vk_template_register(&c,&a,3,h,0,0,0,0,&e,1));assert(pw_vk_template_lookup(&c,3,h,&n));
 assert(!pw_vk_template_register(&c,&a,3,h,1,0,0,0,&e,1));assert(!pw_vk_template_lookup(&c,3,h,&n));fail=0;
 assert(c.count==PW_VK_TEMPLATE_BUCKETS*4u-1);
 pw_vk_template_remove_device(&c,&a,3);assert(c.count==PW_VK_TEMPLATE_BUCKETS*2u);
 for(size_t i=1;i<PW_VK_TEMPLATE_BUCKETS*4u;i+=2)assert(pw_vk_template_lookup(&c,4,UINT64_C(0x100000000)+i,&n));
 pw_vk_template_remove_device(&c,&a,4);assert(!c.count);
 for(size_t i=0;i<PW_VK_TEMPLATE_BUCKETS;i++)assert(!c.buckets[i]);
}

static void codec_cases(void)
{
 unsigned char output[4096], saved[4096], source[128]; size_t bytes, extent;
 uint64_t sets[]={UINT64_C(0x1234567887654321),42}, offsets[]={80,160};
 uint32_t dynamic[]={71,72}; struct pw_vk_template_entry image={0,1,16,24};
 assert(pw_vk_wire_draw(output,sizeof(output),0x1234,7,2,3,-5,8,&bytes));
 assert(bytes==24 && (int32_t)pw_vk_wire_u32(output+16)==-5);
 assert(pw_vk_wire_validate(PW_VK_DRAW_INDEXED,output,bytes));
 assert(pw_vk_wire_pipeline(output,sizeof(output),0x1234,0,sets[0],&bytes));
 assert(pw_vk_wire_u64(output+8)==sets[0]);
 assert(pw_vk_wire_index(output,sizeof(output),0x1234,sets[0],66,77,0,1,&bytes));
 assert(pw_vk_wire_validate(PW_VK_BIND_INDEX,output,bytes));
 assert(pw_vk_wire_descriptors(output,sizeof(output),0x1234,0,77,2,2,sets,2,dynamic,&bytes));
 memcpy(saved,output,bytes);sets[0]=999;dynamic[0]=999;
 assert(!memcmp(saved,output,bytes) && pw_vk_wire_u64(output+32)==UINT64_C(0x1234567887654321));
 assert(pw_vk_wire_validate(PW_VK_BIND_DESCRIPTORS,output,bytes));
 assert(pw_vk_wire_vertex2(output,sizeof(output),0x1234,3,2,sets,offsets,NULL,offsets,&bytes));
 memset(sets,0,sizeof(sets));assert(pw_vk_wire_u64(output+16)==999 && pw_vk_wire_u32(output+12)==2);
 assert(pw_vk_wire_validate(PW_VK_BIND_VERTEX2,output,bytes));
 memset(source,0xab,sizeof(source));assert(pw_vk_template_extent(&image,1,&extent) && extent==40);
 assert(pw_vk_wire_template(output,sizeof(output),0x1234,55,66,&image,1,source,&bytes));
 memset(source,0xee,sizeof(source));assert(bytes==72 && output[48]==0xab && output[67]==0xab && output[68]==0);
 assert(pw_vk_wire_validate(PW_VK_UPDATE_TEMPLATE,output,bytes));
 assert(!pw_vk_wire_template(source,sizeof(source),1,1,1,&image,1,source,&bytes));
 assert(!pw_vk_wire_descriptors(output,sizeof(output),1,0,0,0,1,(uint64_t *)output,0,NULL,&bytes));
 assert(!pw_vk_wire_vertex2(output,sizeof(output),1,0,1,(uint64_t *)output,offsets,NULL,NULL,&bytes));
 memset(source,0x5a,16);
 assert(pw_vk_wire_push_constants(output,sizeof(output),0x1234,UINT64_C(0x1234567887654321),3,8,16,source,&bytes));
 memset(source,0xa5,16);
 assert(bytes==40 && pw_vk_wire_u64(output+8)==UINT64_C(0x1234567887654321) && output[24]==0x5a && output[39]==0x5a);
 assert(pw_vk_wire_validate(PW_VK_PUSH_CONSTANTS,output,bytes));
 assert(!pw_vk_wire_validate(PW_VK_PUSH_CONSTANTS,output,bytes-1));
 assert(!pw_vk_wire_push_constants(output,39,1,1,1,0,16,source,&bytes));
 assert(!pw_vk_wire_push_constants(output,sizeof(output),1,1,1,0,16,output,&bytes));
 assert(!pw_vk_wire_push_constants(output,sizeof(output),1,1,1,0,UINT32_MAX,source,&bytes));
 assert(!pw_vk_wire_push_constants(output,sizeof(output),1,1,1,0,1,NULL,&bytes));
 assert(!pw_vk_wire_push_constants(output,sizeof(output),1,1,1,0,16,(void *)(UINTPTR_MAX-8),&bytes));
 assert(!pw_vk_wire_draw(output,23,1,1,1,1,1,1,&bytes));
 assert(!pw_vk_wire_descriptors(output,sizeof(output),1,0,0,0,UINT32_MAX,NULL,0,NULL,&bytes));
 assert(!pw_vk_wire_validate(999,output,24));
 assert(pw_vk_wire_draw(output,sizeof(output),1,1,1,1,1,1,&bytes));
 assert(!pw_vk_wire_validate(PW_VK_DRAW_INDEXED,output,bytes-1));
}
int main(void)
{
 assert(template_cache_cases()==0);hash_cache_cases();codec_cases();
 puts("PASS Vulkan wire ownership, normalization, alias/bounds checks and descriptor-template lifecycle");
 return 0;
}
