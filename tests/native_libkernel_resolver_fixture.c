/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "native_libkernel_resolver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void write64(unsigned char *p,uint64_t v){memcpy(p,&v,8);}
static void write32(unsigned char *p,uint32_t v){memcpy(p,&v,4);}
static void rejected(unsigned char *p,size_t len,uintptr_t a,uintptr_t b)
{
    struct native_libkernel_entries e={1,2,3};
    assert(!native_libkernel_resolve(p,len,a,b,&e));
    assert(!e.sigaction && !e.sigreturn && !e.sysarch);
}
int main(void)
{
    unsigned char info[0x160]={0},bad[0x160];
    const unsigned char id[20]={0x1c,0x44,0xcc,0xec,0xca,0x33,0x10,0x48,0xdb,0xd9,0x07,0x9f,0xcc,0x8c,0x5c,0x37,0,0,0,0};
    const uint32_t off[]={0,0x48000,0x68000,0x6c000},size[]={0x48000,0x20000,0x4000,0x38000},prot[]={4,1,1,3};
    const uintptr_t base=0x800000000,sa=base+0xc3f0,fs=base+0xde0;
    struct native_libkernel_entries e;
    memcpy(info+8,"libkernel.sprx",14);memcpy(info+0x14c,id,20);write32(info+0x148,4);
    for(unsigned i=0;i<4;i++){write64(info+0x108+i*16,base+off[i]);write32(info+0x110+i*16,size[i]);write32(info+0x114+i*16,prot[i]);}
    assert(native_libkernel_resolve(info,sizeof(info),sa,fs,&e));
    assert(e.sigaction==base+0x1600 && e.sigreturn==base+0x2a0 && e.sysarch==fs);
    write64(info,0x160);assert(native_libkernel_resolve(info,sizeof(info),sa,fs,&e));
    rejected(NULL,sizeof(info),sa,fs);rejected(info,sizeof(info)-1,sa,fs);
    rejected(info,sizeof(info),base+0xd100,base+0x1010); /* ELF imports */
    rejected(info,sizeof(info),sa+1,fs);rejected(info,sizeof(info),sa,fs+1);
    const unsigned mutations[]={0,8,0x108,0x110,0x114,0x118,0x120,0x124,0x128,0x130,0x134,0x138,0x140,0x144,0x148,0x14c,0x15f};
    for(unsigned i=0;i<sizeof(mutations)/sizeof(mutations[0]);i++)
    {memcpy(bad,info,sizeof(bad));bad[mutations[i]]^=1;rejected(bad,sizeof(bad),sa,fs);}
    memcpy(bad,info,sizeof(bad));memset(bad+0x14c,0,20);rejected(bad,sizeof(bad),sa,fs);
    assert(!native_libkernel_resolve(info,sizeof(info),sa,fs,NULL));
    puts("native libkernel resolver: identity accepted; unknown, malformed and ELF identities rejected");
    return 0;
}
