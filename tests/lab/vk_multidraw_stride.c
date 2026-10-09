/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* PE32 produces owned packets; Unix64 checks the records a driver would read. */
#ifdef WINE_UNIX_LIB
#include "vulkan_private.h"
#else
#include "vulkan_loader.h"
#endif
#include "pw_vk_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char wire[1024];
static uint64_t arena[1024];
static void check(unsigned indexed,unsigned count,unsigned override,void *params)
{
 unsigned stride;const unsigned char *draws;
 if(indexed){
  struct vkCmdDrawMultiIndexedEXT_params *p=params;
  assert(p->drawCount==count);assert(p->instanceCount==3);assert(p->firstInstance==7);
  stride=p->stride;draws=(const void *)p->pIndexInfo;
  assert(stride==sizeof(VkMultiDrawIndexedInfoEXT));
  assert(!!p->pVertexOffset==!!override);if(override)assert(*p->pVertexOffset==-17);
 }else{
  struct vkCmdDrawMultiEXT_params *p=params;
  assert(p->drawCount==count);assert(p->instanceCount==3);assert(p->firstInstance==7);
  stride=p->stride;draws=(const void *)p->pVertexInfo;
  assert(stride==sizeof(VkMultiDrawInfoEXT));
 }
 for(unsigned i=0;i<count;i++){
  if(indexed){VkMultiDrawIndexedInfoEXT d;memcpy(&d,draws+i*stride,sizeof(d));
   assert(d.firstIndex==11*(i+1));assert(d.indexCount==3*(i+1));
   if(!override)assert(d.vertexOffset==-(int)(i+1));
  }else{VkMultiDrawInfoEXT d;memcpy(&d,draws+i*stride,sizeof(d));
   assert(d.firstVertex==11*(i+1));assert(d.vertexCount==3*(i+1));}
 }
}
int main(int argc,char **argv)
{
 assert(argc==3);unsigned produce=!strcmp(argv[1],"produce");
 FILE *f=fopen(argv[2],produce?"wb":"rb");assert(f);
 for(unsigned indexed=0;indexed<2;indexed++)for(unsigned variant=0;variant<6;variant++){
  unsigned count=(variant==0 || variant==5)?0:variant==1?1:3;
  unsigned override=indexed && variant==4;
  unsigned code=indexed?unix_vkCmdDrawMultiIndexedEXT:unix_vkCmdDrawMultiEXT;
  uint32_t length;void *decoded;
  if(produce){
   unsigned char source[96];memset(source,0xab,sizeof(source));
   unsigned element=indexed?sizeof(VkMultiDrawIndexedInfoEXT):sizeof(VkMultiDrawInfoEXT);
   unsigned stride=variant<2?0:variant==2?element:element+8;
   int32_t offset=-17;size_t bytes;
   struct vkCmdDrawMultiEXT_params p={0};struct vkCmdDrawMultiIndexedEXT_params q={0};
   for(unsigned i=0;i<count;i++){
    VkMultiDrawIndexedInfoEXT d={11*(i+1),3*(i+1),-(int)(i+1)};
    memcpy(source+i*stride,&d,element);
   }
   p.commandBuffer=(VkCommandBuffer)(uintptr_t)9;p.drawCount=count;p.pVertexInfo=(const void *)source;p.instanceCount=3;p.firstInstance=7;p.stride=stride;
   q.commandBuffer=p.commandBuffer;q.drawCount=count;q.pIndexInfo=(const void *)source;q.instanceCount=3;q.firstInstance=7;q.stride=stride;q.pVertexOffset=override?&offset:NULL;
   if(variant==5){p.pVertexInfo=NULL;q.pIndexInfo=NULL;}
   void *input=indexed?(void *)&q:(void *)&p;
   assert(pw_vk_generated_encode(code,input,wire,sizeof(wire),&bytes));
   assert(p.stride==stride && q.stride==stride);
   assert(p.pVertexInfo==(variant==5?NULL:(const void *)source) && q.pIndexInfo==(variant==5?NULL:(const void *)source));
   memset(source,0xee,sizeof(source));offset=99;length=bytes;
   assert(fwrite(&length,sizeof(length),1,f)==1);assert(fwrite(wire,1,length,f)==length);
   if(count>1){
    p.stride=q.stride=0;assert(!pw_vk_generated_encode(code,input,wire,sizeof(wire),&bytes));
    p.stride=q.stride=element-4;assert(!pw_vk_generated_encode(code,input,wire,sizeof(wire),&bytes));
    p.stride=q.stride=element+1;assert(!pw_vk_generated_encode(code,input,wire,sizeof(wire),&bytes));
    p.stride=q.stride=stride;p.pVertexInfo=(const void *)(uintptr_t)(UINTPTR_MAX-3);q.pIndexInfo=(const void *)p.pVertexInfo;
    assert(!pw_vk_generated_encode(code,input,wire,sizeof(wire),&bytes));
   }
  }else{
   assert(fread(&length,sizeof(length),1,f)==1);assert(length<=sizeof(wire));assert(fread(wire,1,length,f)==length);
   assert(pw_vk_generated_decode(code,wire,length,arena,sizeof(arena),&decoded));check(indexed,count,override,decoded);
   assert(!pw_vk_generated_decode(code,wire,length-1,arena,sizeof(arena),&decoded));
  }
 }
 if(!produce)assert(fgetc(f)==EOF);
 assert(!fclose(f));puts("PASS multidraw stride ownership and Vulkan-visible values");return 0;
}
