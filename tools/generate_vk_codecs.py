#!/usr/bin/env python3
"""Generate owned Vulkan input codecs from the exact Wine Vulkan type model.

The generator emits every void thunk in the pinned Unix enum, with an explicit
manifest for immediate outputs, callbacks, PE-owned lifecycle, and unsupported
input shapes. An unsupported nested type fails encoding before enqueue; it is
never copied as an opaque pointer.
"""
import argparse,contextlib,io,json,os,pathlib,re,runpy,logging

class Codecs:
 def __init__(self,model,names):
  self.m=model;self.T=model['Type'];self.R=model['Record'];self.F=model['Function'];self.records={};self.reasons={};self.names=names
 def type(self,name):return self.T.get(name)
 def record(self,t):return isinstance(t,self.R)
 def reachable(self,t):
  if not self.record(t) or t.name in self.records:return
  self.records[t.name]=t
  for m in t.members:self.reachable(m.type)
 def plain(self,t,seen=None):
  if not self.record(t):return not isinstance(t,self.m['FunctionPointer']) and not(isinstance(t,self.m['Handle']) and t.is_dispatchable())
  seen=set() if seen is None else seen
  if t.name in seen:return False
  return all(not m.is_pointer() and self.plain(m.type,seen|{t.name}) for m in t.members)
 def reason(self,parent,why):self.reasons.setdefault(parent,[]).append(why)
 def count(self,v,container):
  try:
   if isinstance(v,self.m['Parameter']):return str(v.get_dyn_array_len(container,'p->',False))
   return str(v.get_dyn_array_len('p->',False))
  except (KeyError,ValueError,AttributeError) as e:
   if v.type_name=='uint32_t' and v.dyn_array_len==r'latexmath:[\textrm{codeSize} \over 4]':return 'p->codeSize/4'
   if v.name=='pSampleMask':return '(p->rasterizationSamples+31)/32' if getattr(container,'name',None)=='VkPipelineMultisampleStateCreateInfo' else '(p->samples+31)/32'
   self.reason(container.name if hasattr(container,'name') else v.name,'unsupported length '+str(v.dyn_array_len));return None
 def scalar(self,v,address):
  wide=v.is_pointer_size() or v.type_name=='size_t'
  return f'CHECK(pw_vk_codec_value(c,(void *)({address}),sizeof(*({address})),{int(wide)}));'
 def element(self,v,address,selector='0'):
  if self.record(v.type):return f'CHECK(codec_{v.type.name}(c,(void *)({address}),{selector}));'
  if isinstance(v.type,self.m['FunctionPointer']):
   self.reason(getattr(getattr(v,'parent',None),'name',v.name),'guest callback '+v.name)
   return 'goto fail; /* callback pointer cannot be deferred */'
  return self.scalar(v,address)
 def field(self,v,parent):
  address='&p->'+v.name
  if getattr(v,'bit_width',None):
   width=v.bit_width
   return '{ uint64_t bits=c->decode?0:(uint64_t)p->'+v.name+'; CHECK(pw_vk_codec_value(c,&bits,8,0)); if(bits>UINT64_C('+str((1<<width)-1)+'))goto fail; if(c->decode)p->'+v.name+'=bits; }'
  if v.name=='pData' and ('DescriptorSetWithTemplate' in parent.name):return f'CHECK(pw_vk_codec_template(c,(void *)({address}),p->descriptorUpdateTemplate));'
  if v.name=='pCheckpointMarker':return f'CHECK(pw_vk_codec_value(c,(void *)({address}),sizeof(p->{v.name}),1)); /* opaque marker identity: never dereferenced */'
  if v.name=='pAllocator':return f'if(p->{v.name})goto fail; CHECK(pw_vk_codec_array(c,(void *)({address}),0,1,1)==0);'
  if v.name=='pNext':return f'CHECK(codec_next(c,(void *)({address})));'
  selector='p->'+v.selector if v.selector else '0'
  if self.pointer(v):
   if v.dyn_array_len=='null-terminated':return f'CHECK(pw_vk_codec_string(c,(void *)({address})));'
   if v.is_pointer_pointer() or v.pointer_array:
    if v.type_name=='void':self.reason(parent.name,'unbounded pointer-of-pointer '+v.name);return 'goto fail; /* input blob lengths unavailable */'
    count=self.extent(v,parent)
    if count is None:return 'goto fail;'
    inner='p->pInfos[i].geometryCount' if v.name in ['ppBuildRangeInfos','ppMaxPrimitiveCounts'] else '1'
    return '{ uint64_t count=(uint64_t)('+count+'); int present=pw_vk_codec_array(c,(void *)('+address+'),count,sizeof(void *),'+str(int(bool(v.optional)))+'); if(present<0)goto fail; if(present){ for(uint64_t i=0;i<count;i++){ int child=pw_vk_codec_array(c,(void *)&p->'+v.name+'[i],(uint64_t)('+inner+'),sizeof('+v.type_name+'),0); if(child<0)goto fail;if(child){for(uint64_t j=0;j<(uint64_t)('+inner+');j++){'+self.element(v,'&p->'+v.name+'[i][j]',selector)+'}} } } }'
   if v.type_name=='void' and (not v.dyn_array_len or isinstance(v.dyn_array_len,int)):
    self.reason(parent.name,'opaque pointer '+v.name);return 'goto fail; /* no input byte extent */'
   count=self.extent(v,parent)
   if count is None:return 'goto fail;'
   typ='uint8_t' if v.type_name=='void' else v.type_name
   optional=int(bool(v.optional))
   out=f'{{ uint64_t count=(uint64_t)({count}); int present=pw_vk_codec_array(c,(void *)({address}),count,sizeof({typ}),{optional}); if(present<0)goto fail; if(present){{'
   out+=f'for(uint64_t i=0;i<count;i++){{'+self.element(v,f'&(({typ} *)p->{v.name})[i]',selector) if v.type_name!='void' else f'for(uint64_t i=0;i<count;i++){{ CHECK(pw_vk_codec_value(c,&((uint8_t *)p->{v.name})[i],1,0));'
   return out+'}} }'
  if v.array_lens:
   typ=v.type_name
   return '{ for(size_t i=0;i<sizeof(p->'+v.name+')/sizeof('+typ+');i++){'+self.element(v,'&(('+typ+' *)p->'+v.name+')[i]',selector)+'} }'
  return self.element(v,address,selector)
 def pointer(self,v):return v.is_pointer() or (isinstance(v,self.m['Parameter']) and bool(v.array_lens))
 def extent(self,v,parent):
  if isinstance(v,self.m['Parameter']) and v.array_lens:return '*'.join('('+str(n)+')' for n in v.array_lens)
  return self.count(v,parent.params if isinstance(parent,self.F) else parent) if v.dyn_array_len else '1'
 def ordered(self,fields):return sorted(fields,key=lambda v:int(bool(self.pointer(v))))
 def body(self,t):
  if t.name in ['VkDeviceOrHostAddressKHR','VkDeviceOrHostAddressConstKHR']:
   return 'if(!c->gpu_addresses)goto fail; CHECK(pw_vk_codec_value(c,&p->deviceAddress,sizeof(p->deviceAddress),0));'
  if t.union:
   if self.plain(t):return 'for(size_t i=0;i<sizeof(*p);i++)CHECK(pw_vk_codec_value(c,((uint8_t *)p)+i,1,0));'
   if all(m.selection for m in t.members):
    out='switch(selector){'
    for m in t.members:
     out+=''.join('case '+v+':' for v in (m.selection if isinstance(m.selection,list) else m.selection.split(',')))+'{'+self.field(m,t)+'break;}'
    return out+'default:goto fail;}'
   self.reason(t.name,'union requires unavailable discriminant');return 'goto fail;'
  out=''
  if t.name=='VkWriteDescriptorSet':
   out+='''switch(p->descriptorType){
case VK_DESCRIPTOR_TYPE_SAMPLER:case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:break;
case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:break;
case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:break;
case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:break;
default:goto fail;}\n'''
   # Type is only known after prior scalar decode, so place validation at end.
   validate=out;out=''
  for v in self.ordered(t.members):
   stmt=self.field(v,t)
   if t.name=='VkWriteDescriptorSet' and v.name in ['pImageInfo','pBufferInfo','pTexelBufferView']:
    conditions={'pImageInfo':'p->descriptorType<=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || p->descriptorType==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT','pBufferInfo':'p->descriptorType>=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && p->descriptorType<=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC','pTexelBufferView':'p->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || p->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER'}
    # Do not even read inactive API members; their values may be garbage.
    out+='if('+conditions[v.name]+'){'+stmt+'}else if(c->decode){'+v.type_name+' *nil=NULL;memcpy((void *)&p->'+v.name+',&nil,sizeof(nil));}\n'
   else:out+=stmt+'\n'
  if t.name=='VkWriteDescriptorSet':out+=validate
  if t.chain_type and t.chain_type.values:out+='if(p->sType!='+t.chain_type.values+')goto fail;'
  return out
 def wrapper(self,name,ctype,body,gpu=None):
  return f'''static int __attribute__((unused)) codec_{name}(struct pw_vk_codec *c,void *value,uint64_t selector)
{{ {ctype} *p=value;(void)p;(void)selector;
 if(c->depth>=64)return 0;
 c->depth++;
 {body}
 c->depth--;return 1;
 fail:c->depth--;return 0;
}}\n'''
 def generate(self):
  funcs=[f for f in self.T.all(self.F,self.F.needs_thunk) if f.name in self.names and f.type=='void']
  skip={};active=[]
  for f in funcs:
   if f.name in ['vkDebugReportMessageEXT','vkSubmitDebugUtilsMessageEXT']:skip[f.name]='callback delivery'
   elif any(p.is_pointer() and not p.is_const() for p in f.params):skip[f.name]='immediate output storage'
   else:
    active.append(f)
    for p in f.params:
     if p.name!='pAllocator':self.reachable(p.type)
  # Every registered input pNext type may be reached from a compatible input.
  for t in self.T.all(self.R):
   if t.structextends and not t.returnedonly and t.chain_type and t.chain_type.values:self.reachable(t)
  src='''/* Generated by tools/generate_vk_codecs.py. Do not edit. */
#ifdef WINE_UNIX_LIB
#include "vulkan_private.h"
#else
#include "vulkan_loader.h"
#endif
#include "pw_vk_codec.h"
#include <string.h>
#define CHECK(x) do {if(!(x))goto fail;}while(0)
static int codec_next(struct pw_vk_codec *,void *);
'''
  for name in self.records:src+=f'static int codec_{name}(struct pw_vk_codec *,void *,uint64_t);\n'
  for t in self.records.values():src+=self.wrapper(t.name,t.name,self.body(t))
  nexts={}
  for t in self.records.values():
   if t.structextends and t.chain_type and t.chain_type.values:nexts.setdefault(t.chain_type.values,t)
  src+='''static int codec_next(struct pw_vk_codec *c,void *slot)
{ const VkBaseInStructure *p=NULL;uint32_t type=0;int present;memcpy(&p,slot,sizeof(p));
 if(!c->decode&&p){
  if(!pw_vk_codec_source(c,p,sizeof(VkBaseInStructure)))return 0;
  type=p->sType;if(!type)return 0;
  switch(type){\n'''
  for typ,t in nexts.items():src+=f'case {typ}:if(!pw_vk_codec_source(c,p,sizeof({t.name})))return 0;break;\n'
  src+='''default:return 0;}}
 if(!pw_vk_codec_value(c,&type,4,0))return 0;
 if(!type){p=NULL;if(c->decode)memcpy(slot,&p,sizeof(p));return 1;}
 switch(type){\n'''
  for typ,t in nexts.items():src+=f'case {typ}:present=pw_vk_codec_array(c,slot,1,sizeof({t.name}),0);if(present!=1)return 0;memcpy(&p,slot,sizeof(p));return codec_{t.name}(c,(void *)p,0);\n'
  src+='default:return 0;}}\n'
  for f in active:
   body=''.join(self.field(p,f)+'\n' for p in self.ordered(f.params))
   src+=self.wrapper(f.name,'struct '+f.name+'_params',body)
  src+='size_t pw_vk_generated_param_size(unsigned code){switch(code){\n'
  for f in active:src+=f'case unix_{f.name}:return sizeof(struct {f.name}_params);\n'
  src+='default:return 0;}}\nstatic int generated(struct pw_vk_codec *c,unsigned code,void *args){switch(code){\n'
  for f in active:src+=f'case unix_{f.name}:c->gpu_addresses={int(f.name.startswith("vkCmd"))};return codec_{f.name}(c,args,0);\n'
  src+='default:return 0;}}\n'
  src+='''int pw_vk_generated_encode_templates(unsigned code,void *args,void *wire,size_t capacity,size_t *written,pw_vk_codec_snapshot_fn snapshot,void *context)
{ struct pw_vk_codec c={0};c.wire=wire;c.capacity=capacity;c.template_snapshot=snapshot;c.snapshot_context=context;*written=0;
 if(!args||!wire||capacity>PW_VK_CODEC_MAX_BYTES||!pw_vk_codec_source(&c,args,pw_vk_generated_param_size(code)))return 0;
 if(!generated(&c,code,args))return 0;
 *written=c.used;return 1;}
int pw_vk_generated_encode(unsigned code,void *args,void *wire,size_t capacity,size_t *written)
{ return pw_vk_generated_encode_templates(code,args,wire,capacity,written,NULL,NULL); }
int pw_vk_generated_decode(unsigned code,const void *wire,size_t bytes,void *arena,size_t capacity,void **args)
{ struct pw_vk_codec c={0};size_t size=pw_vk_generated_param_size(code);
 if(!size||size>capacity||bytes>PW_VK_CODEC_MAX_BYTES||capacity>PW_VK_CODEC_DECODE_BYTES||(uintptr_t)arena%8||!wire||!arena)return 0;
 if((uintptr_t)wire>UINTPTR_MAX-bytes||(uintptr_t)arena>UINTPTR_MAX-capacity)return 0;
 if((uintptr_t)wire<(uintptr_t)arena+capacity&&(uintptr_t)arena<(uintptr_t)wire+bytes)return 0;
 c.wire=(unsigned char *)wire;c.capacity=bytes;c.decode=1;c.arena=arena;c.arena_capacity=capacity;c.arena_used=(size+7)&~(size_t)7;memset(arena,0,size);
 if(!generated(&c,code,arena)||c.used!=bytes)return 0;
 *args=arena;return 1;}
'''
  audit=[]
  for f in funcs:
   audit.append({'function':f.name,'codec_generated':f in active,'synchronous_reason':skip.get(f.name),'parameter_shape_reasons':self.reasons.get(f.name,[])})
  src=src.replace(')goto fail;','){goto fail;}')
  self.active=active
  return src,{'schema_version':1,'void_thunks':len(funcs),'generated_thunks':len(active),'functions':audit,'unsupported_structures':self.reasons,'pnext_structures':len(nexts)}
 def fixture_element(self,v,address):
  if self.record(v.type):return f'fill_{v.type.name}((void *)({address}));'
  if v.is_handle():
   literal='0x12345678' if v.handle.is_dispatchable() else 'UINT64_C(0xfedcba9876543210)'
   return f'*({address})=({v.type_name})(uintptr_t)({literal});' if v.handle.is_dispatchable() else f'*({address})=({v.type_name})({literal});'
  if isinstance(v.type,self.m['FunctionPointer']):return f'*({address})=NULL;'
  if v.type_name=='VkSampleCountFlagBits':return f'*({address})=VK_SAMPLE_COUNT_4_BIT;'
  if v.is_enum():return f'*({address})=({v.type_name})0;'
  if v.type_name in ['float','double']:return f'*({address})=({v.type_name})2.5;'
  return f'*({address})=({v.type_name})4;'
 def fixture_field(self,v,parent):
  if v.name in ['pNext','pAllocator']:return ''
  if v.name=='sType' and getattr(v,'values',None):return 'p->sType='+v.values+';'
  if getattr(v,'bit_width',None):return 'p->'+v.name+'=1;'
  address='&p->'+v.name
  selector_init=''
  if v.selector and self.record(v.type) and v.type.union and v.type.members[0].selection:
   selector_init='p->'+v.selector+'='+v.type.members[0].selection[0]+';'
  if v.name=='pData' and ('DescriptorSetWithTemplate' in parent.name):return '{ VkDescriptorBufferInfo *info=allocate(sizeof(*info));info->buffer=UINT64_C(0xfedcba9876543210);info->offset=4;info->range=4;p->pData=info; }'
  if v.name=='pCheckpointMarker':return 'p->pCheckpointMarker=(const void *)(uintptr_t)0x12345678;'
  if self.pointer(v):
   if v.dyn_array_len=='null-terminated':return 'p->'+v.name+'="owned test string";'
   if v.type_name=='void' and (not v.dyn_array_len or isinstance(v.dyn_array_len,int)):return ''
   count=self.extent(v,parent)
   if count is None:return ''
   typ='uint8_t' if v.type_name=='void' else v.type_name
   if v.is_pointer_pointer() or v.pointer_array:
    if v.type_name=='void':return ''
    inner='p->pInfos[i].geometryCount' if v.name in ['ppBuildRangeInfos','ppMaxPrimitiveCounts'] else '1'
    return '{ size_t n=(size_t)('+count+');void **a=allocate(n*sizeof(void *));p->'+v.name+'=(void *)a;for(size_t i=0;i<n;i++){a[i]=allocate((size_t)('+inner+')*sizeof('+typ+'));for(size_t j=0;j<(size_t)('+inner+');j++){'+self.fixture_element(v,'&(('+typ+' *)a[i])[j]')+'}}}'
   if v.type_name=='void':element=f'(({typ} *)p->{v.name})[i]=(uint8_t)(i+3);'
   else:element=self.fixture_element(v,'&(('+typ+' *)p->'+v.name+')[i]')
   return '{ size_t n=(size_t)('+count+');p->'+v.name+'=allocate(n*sizeof('+typ+'));for(size_t i=0;i<n;i++){'+element+'}}'
  if v.array_lens:return '{for(size_t i=0;i<sizeof(p->'+v.name+')/sizeof('+v.type_name+');i++){'+self.fixture_element(v,'&(('+v.type_name+' *)p->'+v.name+')[i]')+'}}'
  return selector_init+self.fixture_element(v,address)
 def fixture(self,active):
  src='''/* Generated codec roundtrip fixtures: no Vulkan driver or console access. */
#ifdef WINE_UNIX_LIB
#include "vulkan_private.h"
#else
#include "vulkan_loader.h"
#endif
#include "pw_vk_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static union{uint64_t align;unsigned char bytes[16*1024*1024];} storage;
static size_t used;static unsigned depth;
static int snapshot(void *context,uint64_t handle,const void *data,void *output,size_t capacity,size_t *written)
{ unsigned char *p=output;uint32_t span=24;(void)context;if(!data||capacity<56)return 0;memset(p,0,56);memcpy(p+16,&handle,8);memcpy(p+24,&span,4);memcpy(p+32,data,24);*written=56;return 1; }

static void *allocate(size_t size){size_t pos=(used+7)&~(size_t)7;if(pos>sizeof(storage.bytes)||size>sizeof(storage.bytes)-pos)abort();void *p=storage.bytes+pos;used=pos+size;memset(p,0,size);return p;}
'''
  for name in self.records:src+=f'static void __attribute__((unused)) fill_{name}(void *);\n'
  for t in self.records.values():
   src+=f'static void __attribute__((unused)) fill_{t.name}(void *value){{ {t.name} *p=value;(void)p;if(depth++>=32)abort();'
   if t.union:
    if t.name in ['VkDeviceOrHostAddressKHR','VkDeviceOrHostAddressConstKHR']:src+='p->deviceAddress=UINT64_C(0x123456789abcdef0);'
    elif self.plain(t):src+='memset(p,0x5a,sizeof(*p));'
    elif all(m.selection for m in t.members):src+=self.fixture_field(t.members[0],t)
   else:
    for v in self.ordered(t.members):src+=self.fixture_field(v,t)
   src+='depth--;}\n'
  for f in active:
   src+=f'static void fill_{f.name}(void *value){{struct {f.name}_params *p=value;(void)p;'+''.join(self.fixture_field(v,f) for v in self.ordered(f.params))+'}\n'
  src+='''int main(int argc,char **argv){unsigned passed=0,unsupported=0;unsigned char wire[262144],again[262144];
 static union{uint64_t align;unsigned char bytes[4*1024*1024];} arena;
 if(argc!=3)return 2;
 int produce=!strcmp(argv[1],"produce");
'''
  for f in active:
   src+='{size_t n=0,m=0;void *decoded=NULL;FILE *file;char path[2048];used=0;depth=0;\n'
   src+=f'struct {f.name}_params *params=allocate(sizeof(*params));fill_{f.name}(params);\n'
   src+=f'snprintf(path,sizeof(path),"%s/{f.name}.bin",argv[2]);\n'
   typed={
    'vkCmdPipelineBarrier2':'struct vkCmdPipelineBarrier2_params *q=decoded;if(q->pDependencyInfo->memoryBarrierCount!=4||q->pDependencyInfo->pMemoryBarriers[0].srcStageMask!=4||q->pDependencyInfo->pBufferMemoryBarriers[0].buffer!=UINT64_C(0xfedcba9876543210))return 10;',
    'vkUpdateDescriptorSets':'struct vkUpdateDescriptorSets_params *q=decoded;if(q->descriptorWriteCount!=4||q->pDescriptorWrites[0].pImageInfo[0].sampler!=UINT64_C(0xfedcba9876543210)||q->pDescriptorWrites[0].pBufferInfo||q->pDescriptorWrites[0].pTexelBufferView)return 11;',
    'vkCmdBeginRendering':'struct vkCmdBeginRendering_params *q=decoded;if(q->pRenderingInfo->colorAttachmentCount!=4||q->pRenderingInfo->pColorAttachments[0].imageView!=UINT64_C(0xfedcba9876543210))return 12;',
    'vkCmdCopyImage2':'struct vkCmdCopyImage2_params *q=decoded;if(q->pCopyImageInfo->regionCount!=4||q->pCopyImageInfo->srcImage!=UINT64_C(0xfedcba9876543210))return 13;',
    'vkCmdPushDataEXT':'struct vkCmdPushDataEXT_params *q=decoded;if(q->pPushDataInfo->data.size!=4||((const uint8_t *)q->pPushDataInfo->data.address)[0]!=3)return 14;',
   }.get(f.name,'')
   src+=f'''if(!pw_vk_generated_encode_templates(unix_{f.name},params,wire,sizeof(wire),&n,snapshot,NULL)){{unsupported++;printf("UNSUPPORTED {f.name}\\n");}}
 else{{
 if(produce){{file=fopen(path,"wb");if(!file||fwrite(wire,1,n,file)!=n)return 3;fclose(file);}}
 else{{file=fopen(path,"rb");if(!file)return 4;size_t got=fread(again,1,sizeof(again),file);fclose(file);if(got!=n||memcmp(wire,again,n)){{fprintf(stderr,"cross ABI mismatch {f.name}\\n");return 5;}}}}
 memset(storage.bytes,0xcc,used);
 if(!pw_vk_generated_decode(unix_{f.name},wire,n,arena.bytes,sizeof(arena.bytes),&decoded)){{fprintf(stderr,"decode failed {f.name}\\n");return 6;}}
 {typed}
 if(!pw_vk_generated_encode_templates(unix_{f.name},decoded,again,sizeof(again),&m,snapshot,NULL)||m!=n||memcmp(wire,again,n)){{fprintf(stderr,"roundtrip failed {f.name}\\n");return 7;}}
 if(n&&pw_vk_generated_decode(unix_{f.name},wire,n-1,arena.bytes,sizeof(arena.bytes),&decoded))return 8;
 if(n<sizeof(wire)){{wire[n]=0;if(pw_vk_generated_decode(unix_{f.name},wire,n+1,arena.bytes,sizeof(arena.bytes),&decoded))return 9;}}
 passed++;}}}}\n'''
  return src+'printf("PASS generated roundtrips=%u unsupported_shapes=%u\\n",passed,unsupported);return 0;}\n'

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--source',required=True);ap.add_argument('--xml');ap.add_argument('--video-xml');ap.add_argument('--output',required=True);a=ap.parse_args();source=pathlib.Path(a.source).resolve();out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True)
 logging.disable(logging.CRITICAL)
 with contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
  model=runpy.run_path(str(source/'dlls/winevulkan/make_vulkan'));cache=pathlib.Path(os.environ.get('XDG_CACHE_HOME',str(pathlib.Path.home()/'.cache')))/'wine';cache.mkdir(parents=True,exist_ok=True);version=model['VK_XML_VERSION']
  xml=pathlib.Path(a.xml).resolve() if a.xml else cache/('vk-'+version+'.xml');video=pathlib.Path(a.video_xml).resolve() if a.video_xml else cache/('video-'+version+'.xml')
  if not a.xml:model['download_vk_xml'](str(xml),'vk.xml')
  if not a.video_xml:model['download_vk_xml'](str(video),'video.xml')
  previous=os.getcwd();os.chdir(source/'dlls/winevulkan')
  try:model['Generator'](str(xml),str(video))
  finally:os.chdir(previous)
 names=set(re.findall(r'\bunix_(vk\w+),',(source/'dlls/winevulkan/loader_thunks.h').read_text()));codecs=Codecs(model,names);src,audit=codecs.generate();(out/'pw_vk_generated_roundtrip.c').write_text(codecs.fixture(codecs.active));(out/'pw_vk_generated.c').write_text(src);(out/'pw_vk_generated_manifest.json').write_text(json.dumps(audit,indent=2)+'\n');print(json.dumps({k:v for k,v in audit.items() if k not in ['functions','unsupported_structures']}))
if __name__=='__main__':main()
