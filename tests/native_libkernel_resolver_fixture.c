/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "native_libkernel_resolver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void write64(unsigned char *p,uint64_t v){memcpy(p,&v,8);}
static void write32(unsigned char *p,uint32_t v){memcpy(p,&v,4);}

/* A model of the kernel behind libkernel's stubs: only the syscall
 * instruction of the model's getppid stub runs system calls. */
static const uintptr_t base=0x800000000,getppid_stub=base+0x6c0,sysarch_stub=base+0xde0;
static uintptr_t registered=base+0xc5f0,stub=getppid_stub+10;
static long calls,pid_answer=4590;
static long model_syscall(uintptr_t entry,long number,long a1,long a2,long a3)
{
    calls++;
    if(entry!=stub) return -1;
    if(number==20) return pid_answer;
    if(number==39) return 55;
    if(number==416&&a1==11&&!a2&&a3){uintptr_t *h=(uintptr_t *)(uintptr_t)a3;*h=registered;return 0;}
    return -1;
}
static const struct native_libkernel_ops good={model_syscall,4590,55};

static void rejected(const unsigned char *p,size_t len,uintptr_t g,uintptr_t s,const struct native_libkernel_ops *ops)
{
    struct native_libkernel_entries e={1,2};
    assert(!native_libkernel_resolve(p,len,g,s,ops,&e));
    assert(!e.syscall && !e.sysarch);
}
int main(void)
{
    unsigned char info[0x160]={0},bad[0x160];
    /* the layout measured on the owner's console; the resolver may not depend on it */
    const uint32_t off[]={0,0x48000,0x68000,0x6c000},size[]={0x48000,0x20000,0x4000,0x38000},prot[]={4,1,1,3};
    struct native_libkernel_entries e;
    struct native_libkernel_ops ops;
    memcpy(info+8,"libkernel.sprx",14);write32(info+0x148,4);
    for(unsigned i=0;i<4;i++){write64(info+0x108+i*16,base+off[i]);write32(info+0x110+i*16,size[i]);write32(info+0x114+i*16,prot[i]);}

    assert(native_libkernel_resolve(info,sizeof(info),getppid_stub,sysarch_stub,&good,&e));
    assert(e.syscall==getppid_stub+10 && e.sysarch==sysarch_stub);
    write64(info,0x160);assert(native_libkernel_resolve(info,sizeof(info),getppid_stub,sysarch_stub,&good,&e));

    /* any firmware: another fingerprint, a moved and resized text segment */
    memcpy(bad,info,sizeof(bad));memset(bad+0x14c,0x5a,20);
    assert(native_libkernel_resolve(bad,sizeof(bad),getppid_stub,sysarch_stub,&good,&e));
    write64(bad+0x108,base-0x4000);write32(bad+0x110,0x50000);write32(bad+0x114,5);write32(bad+0x148,1);
    assert(native_libkernel_resolve(bad,sizeof(bad),getppid_stub,sysarch_stub,&good,&e));
    /* no handler registered yet */
    registered=0;assert(native_libkernel_resolve(info,sizeof(info),getppid_stub,sysarch_stub,&good,&e));registered=base+0xc5f0;

    /* malformed records */
    rejected(NULL,sizeof(info),getppid_stub,sysarch_stub,&good);
    rejected(info,sizeof(info)-1,getppid_stub,sysarch_stub,&good);
    memcpy(bad,info,sizeof(bad));bad[0]^=1;rejected(bad,sizeof(bad),getppid_stub,sysarch_stub,&good);
    memcpy(bad,info,sizeof(bad));bad[8]^=1;rejected(bad,sizeof(bad),getppid_stub,sysarch_stub,&good); /* other module, e.g. the ELF loader's */
    memcpy(bad,info,sizeof(bad));write32(bad+0x148,0);rejected(bad,sizeof(bad),getppid_stub,sysarch_stub,&good);
    memcpy(bad,info,sizeof(bad));write32(bad+0x148,5);rejected(bad,sizeof(bad),getppid_stub,sysarch_stub,&good);
    memcpy(bad,info,sizeof(bad));write32(bad+0x114,1);rejected(bad,sizeof(bad),getppid_stub,sysarch_stub,&good); /* text not executable */
    memcpy(bad,info,sizeof(bad));write32(bad+0x110,0x6c0);rejected(bad,sizeof(bad),getppid_stub,sysarch_stub,&good); /* stub outside text */

    /* imports and operations */
    rejected(info,sizeof(info),0,sysarch_stub,&good);
    rejected(info,sizeof(info),getppid_stub,0,&good);
    rejected(info,sizeof(info),getppid_stub,sysarch_stub,NULL);
    ops=good;ops.syscall_at=NULL;rejected(info,sizeof(info),getppid_stub,sysarch_stub,&ops);
    ops=good;ops.ppid=ops.pid;rejected(info,sizeof(info),getppid_stub,sysarch_stub,&ops);
    ops=good;ops.pid=0;rejected(info,sizeof(info),getppid_stub,sysarch_stub,&ops);

    /* the instruction must run what %rax asks for */
    stub=getppid_stub+8;rejected(info,sizeof(info),getppid_stub,sysarch_stub,&good);stub=getppid_stub+10;
    pid_answer=55;rejected(info,sizeof(info),getppid_stub,sysarch_stub,&good);pid_answer=4590; /* a getppid-only stub */
    /* a registration outside libkernel's text is not the kernel's view */
    registered=0x100404610;rejected(info,sizeof(info),getppid_stub,sysarch_stub,&good);registered=base+0xc5f0;

    calls=0;assert(!native_libkernel_resolve(info,sizeof(info),getppid_stub,sysarch_stub,&good,NULL));assert(!calls);
    puts("native libkernel resolver: syscall stub accepted on any layout; malformed records, foreign modules and wrong stubs rejected");
    return 0;
}
