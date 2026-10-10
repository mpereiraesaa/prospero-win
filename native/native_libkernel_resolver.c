/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "native_libkernel_resolver.h"
#include <string.h>

static uint64_t read64(const unsigned char *p)
{
    uint64_t v; memcpy(&v,p,sizeof(v)); return v;
}
static uint32_t read32(const unsigned char *p)
{
    uint32_t v; memcpy(&v,p,sizeof(v)); return v;
}
int native_libkernel_resolve(const void *record, size_t length,
                            uintptr_t sigaction_import, uintptr_t sysarch_import,
                            struct native_libkernel_entries *entries)
{
    /* Ordinary-title identity, bound to the external live positive-control
     * scan and in-title registration/fault-return proofs. ELF loader's
     * libkernel is a different module and is deliberately unsupported. */
    static const unsigned char fingerprint[20]={
        0x1c,0x44,0xcc,0xec,0xca,0x33,0x10,0x48,0xdb,0xd9,
        0x07,0x9f,0xcc,0x8c,0x5c,0x37,0,0,0,0
    };
    static const uint32_t offsets[4]={0,0x48000,0x68000,0x6c000};
    static const uint32_t sizes[4]={0x48000,0x20000,0x4000,0x38000};
    static const uint32_t protections[4]={4,1,1,3};
    const unsigned char *info=record;
    uintptr_t base;
    if(!entries) return 0;
    memset(entries,0,sizeof(*entries));
    if(!info || length!=0x160) return 0;
    if(read64(info)!=0 && read64(info)!=0x160) return 0;
    if(memcmp(info+8,"libkernel.sprx",sizeof("libkernel.sprx")) ||
       read32(info+0x148)!=4 || memcmp(info+0x14c,fingerprint,20)) return 0;
    base=read64(info+0x108);
    if(!base || (base&0x3fff) || base>UINTPTR_MAX-0xa4000) return 0;
    for(unsigned i=0;i<4;i++)
    {
        const unsigned char *s=info+0x108+i*16;
        if(read64(s)!=base+offsets[i] || read32(s+8)!=sizes[i] ||
           read32(s+12)!=protections[i]) return 0;
    }
    if(sigaction_import!=base+0xc3f0 || sysarch_import!=base+0xde0) return 0;
    entries->sigaction=base+0x1600;
    entries->sigreturn=base+0x2a0;
    entries->sysarch=sysarch_import;
    return 1;
}
