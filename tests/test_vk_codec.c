/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/vulkan/pw_vk_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static union {uint64_t aligned;unsigned char bytes[4096];} arena;
static unsigned char wire[4096];
static struct pw_vk_codec encoder(void){struct pw_vk_codec c={0};c.wire=wire;c.capacity=sizeof(wire);return c;}
static struct pw_vk_codec decoder(size_t n){struct pw_vk_codec c=encoder();c.decode=1;c.capacity=n;c.arena=arena.bytes;c.arena_capacity=sizeof(arena.bytes);return c;}
static int snapshot(void *context,uint64_t handle,const void *data,void *out,size_t capacity,size_t *written)
{
 unsigned char *p=out;uint32_t bytes=24;(void)context;
 if(!data||capacity<56)return 0;
 memset(p,0,56);memcpy(p+16,&handle,8);memcpy(p+24,&bytes,4);memcpy(p+32,data,24);*written=56;return 1;
}
int main(void)
{
 struct pw_vk_codec c=encoder(),d;uint32_t guest=0xfedcba98;uint64_t native=0;
 assert(pw_vk_codec_value(&c,&guest,4,1));assert(c.used==8);
 d=decoder(c.used);assert(pw_vk_codec_value(&d,&native,8,1));assert(native==guest);
 native=UINT64_MAX;c=encoder();assert(pw_vk_codec_value(&c,&native,8,1));d=decoder(c.used);assert(!pw_vk_codec_value(&d,&guest,4,1));
 char text[]="mutable caller string";const char *p=text,*copied=NULL;
 c=encoder();assert(pw_vk_codec_string(&c,&p));size_t bytes=c.used;memset(text,'x',sizeof(text));d=decoder(bytes);assert(pw_vk_codec_string(&d,&copied));assert(!strcmp(copied,"mutable caller string"));assert((uintptr_t)copied%8==0);
 p=NULL;c=encoder();assert(pw_vk_codec_string(&c,&p));d=decoder(c.used);copied=(void *)1;assert(pw_vk_codec_string(&d,&copied));assert(!copied);
 p=(void *)(UINTPTR_MAX-1);c=encoder();assert(pw_vk_codec_array(&c,&p,4,8,0)==-1);
 memset(wire,0xa5,sizeof(wire));p=(const char *)wire;c=encoder();assert(pw_vk_codec_array(&c,&p,8,8,0)==-1);assert(c.used==0&&wire[0]==0xa5);
 c=encoder();assert(!pw_vk_codec_value(&c,wire+20,4,0));
 p=NULL;c=encoder();assert(pw_vk_codec_array(&c,&p,5,4,0)==-1);c=encoder();assert(pw_vk_codec_array(&c,&p,5,4,1)==0);
 c=encoder();assert(pw_vk_codec_array(&c,&p,UINT64_MAX,8,1)==-1);
 uint32_t bad=2;memcpy(wire,&bad,4);d=decoder(4);assert(pw_vk_codec_array(&d,&p,1,8,0)==-1);
 char raw[]="abc";p=raw;c=encoder();assert(pw_vk_codec_string(&c,&p));bytes=c.used;wire[bytes-1]='x';d=decoder(bytes);assert(!pw_vk_codec_string(&d,&copied));
 c=encoder();c.capacity=1;assert(!pw_vk_codec_value(&c,&guest,4,0));
 uint64_t descriptor[3]={UINT64_C(0xfedcba9876543210),8,64},handle=UINT64_C(0xaabbccdd12345678);
 p=(const char *)descriptor;c=encoder();assert(!pw_vk_codec_template(&c,&p,handle));
 c=encoder();c.template_snapshot=snapshot;assert(pw_vk_codec_template(&c,&p,handle));bytes=c.used;
 memset(descriptor,0xcc,sizeof(descriptor));d=decoder(bytes);copied=NULL;
 assert(pw_vk_codec_template(&d,&copied,handle));assert((uintptr_t)copied%8==0);
 assert(((uint64_t *)copied)[0]==UINT64_C(0xfedcba9876543210)&&((uint64_t *)copied)[1]==8);
 d=decoder(bytes);assert(!pw_vk_codec_template(&d,&copied,handle+1));
 d=decoder(bytes-1);assert(!pw_vk_codec_template(&d,&copied,handle));
 wire[4+28]=1;d=decoder(bytes);assert(!pw_vk_codec_template(&d,&copied,handle));
 wire[4+28]=0;d=decoder(bytes);d.arena_capacity=23;assert(!pw_vk_codec_template(&d,&copied,handle));
 puts("PASS scalar widening, owned strings, nullable arrays, alignment, overflow, alias, malformed/truncated inputs");return 0;
}
