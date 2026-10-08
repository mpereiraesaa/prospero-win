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
 puts("PASS scalar widening, owned strings, nullable arrays, alignment, overflow, alias, malformed/truncated inputs");return 0;
}
