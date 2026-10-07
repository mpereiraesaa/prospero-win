/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_template_cache.h"
#include <assert.h>
#include <stdlib.h>
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
 assert(!pw_vk_template_register(&c,&a,1,h,1,0,0,1,&e,1));
 e.type=1000150000;assert(!pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));e.type=6;
 fail=1;assert(!pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));fail=0;
 assert(pw_vk_template_register(&c,&a,1,h,1,0,0,0,&e,1));assert(pw_vk_template_register(&c,&a,2,h,1,0,0,0,&e,1));
 pw_vk_template_remove_device(&c,&a,1);assert(!pw_vk_template_lookup(&c,1,h,&n));assert(pw_vk_template_lookup(&c,2,h,&n));
 pw_vk_template_remove(&c,&a,2,h);assert(!c.head);
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
  sparse[0].count=1;sparse[0].type=1000138000;assert(!pw_vk_template_extent(sparse,2,&extent));
  pw_vk_template_remove_device(&c,&a,3);assert(!c.head);
 }
 return 0;
}

#include <stdio.h>
#include <string.h>
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
 assert(template_cache_cases()==0);codec_cases();
 puts("PASS Vulkan wire ownership, normalization, alias/bounds checks and descriptor-template lifecycle");
 return 0;
}
