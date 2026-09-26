/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_unix_probe.h"
#include "../wine/ps5/pw_wine_prx.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* A fake ntdll.prx: a descriptor in a data segment whose entries point at
 * the fakes below, and a code segment spanning the fakes. */
static _Alignas(16) uint8_t image[1024];
static PwWineHeapStats heap;
static int starts,start_rc,adopt_ok,malloc_ok,open_ok,sym_ok,no_descriptor;
static int32_t load_rc;
static char loaded[256],adopted[256],opened[256],symbol[64];
static int32_t adopted_handle;
static int open_mode;

static int fake_start(size_t argc,const void *argv){(void)argc;(void)argv;starts++;return start_rc;}
static void *fake_adopt(const char *path,int32_t handle)
{strcpy(adopted,path);adopted_handle=handle;return adopt_ok?image:NULL;}
static void *fake_open(const char *path,int mode){strcpy(opened,path);open_mode=mode;return open_ok?image+512:NULL;}
static void *fake_sym(void *module,const char *name)
{assert(module==image+512);strcpy(symbol,name);return sym_ok?image+600:NULL;}
static char *fake_error(void){return "module not found";}
static void fake_stats(PwWineHeapStats *stats){*stats=heap;}
static void *fake_malloc(size_t bytes)
{if(!malloc_ok){heap.failures++;return NULL;}heap.allocations++;heap.live_bytes+=bytes;return malloc(bytes);}
static void fake_free(void *p){if(p){heap.frees++;free(p);}}

static const struct { const char *name; void (*address)(void); } fakes[]={
    {"module_start",(void (*)(void))fake_start},{"pw_wine_dl_adopt",(void (*)(void))fake_adopt},
    {"dlopen",(void (*)(void))fake_open},{"dlsym",(void (*)(void))fake_sym},
    {"dlerror",(void (*)(void))fake_error},{"pw_wine_heap_stats",(void (*)(void))fake_stats},
    {"pw_wine_heap_malloc",(void (*)(void))fake_malloc},{"pw_wine_heap_free",(void (*)(void))fake_free},
};
enum { FAKES=sizeof(fakes)/sizeof(fakes[0]) };

/* Writes the descriptor, leaving out export `skip` (-1: none). */
static void build_image(int skip)
{
    memset(image,0,sizeof(image));
    PwPrxDescriptor *d=(PwPrxDescriptor *)(void *)image;
    char *names=(char *)image+512;
    d->magic=PW_PRX_MAGIC;d->version=PW_PRX_VERSION;
    for(int i=0;i<FAKES;i++) {
        if(i==skip)continue;
        strcpy(names,fakes[i].name);
        d->exports[d->count].name=names;d->exports[d->count].address=(const void *)(uintptr_t)fakes[i].address;
        d->count++;names+=strlen(fakes[i].name)+1;
    }
}
static int32_t load_start(const char *path,size_t argc,const void *argv,uint32_t flags,
                          const void *option,int *result)
{
    (void)argc;(void)argv;(void)flags;(void)option;strcpy(loaded,path);*result=0;return load_rc;
}
static int module_info(int32_t handle,void *info)
{
    assert(handle==load_rc);
    uint8_t *raw=info;uint64_t size;memcpy(&size,raw,8);assert(size==PW_PRX_MODULE_INFO_BYTES);
    uintptr_t low=UINTPTR_MAX,high=0;
    for(int i=0;i<FAKES;i++) {
        uintptr_t a=(uintptr_t)fakes[i].address;
        if(a<low)low=a;
        if(a>high)high=a;
    }
    uint64_t data=(uintptr_t)(no_descriptor?image+768:image),code=low;
    uint32_t bytes=no_descriptor?256:sizeof(image),rd=1,rx=5,count=2,code_bytes=(uint32_t)(high-low+64);
    memcpy(raw+0x108,&data,8);memcpy(raw+0x110,&bytes,4);memcpy(raw+0x114,&rd,4);
    memcpy(raw+0x118,&code,8);memcpy(raw+0x120,&code_bytes,4);memcpy(raw+0x124,&rx,4);
    memcpy(raw+0x148,&count,4);
    return 0;
}
static const PwWineUnixProbeOps ops={load_start,module_info};

static void reset(void)
{
    build_image(-1);memset(&heap,0,sizeof(heap));
    starts=start_rc=no_descriptor=0;adopt_ok=malloc_ok=open_ok=sym_ok=1;load_rc=7;
    loaded[0]=adopted[0]=opened[0]=symbol[0]=0;adopted_handle=0;open_mode=0;
}
static int run(PwWineUnixProbeReport *r){return pw_wine_unix_probe(&ops,"/app0/sce_module",r);}

int main(void)
{
    PwWineUnixProbeReport r;

    /* Every step, in the order the title will take them. */
    reset();heap.allocations=40;heap.frees=30;
    assert(run(&r)==0 && r.step==PW_WINE_UNIX_STEP_DONE && !strcmp(pw_wine_unix_probe_step_name(r.step),"done"));
    assert(!strcmp(loaded,"/app0/sce_module/ntdll.prx") && r.ntdll_handle==7 && r.start_result==0);
    assert(starts==1 && r.module_start_rc==0 && !r.missing_exports);
    assert(!strcmp(adopted,"/app0/sce_module/ntdll.prx") && adopted_handle==7);
    assert(r.heap_before.allocations==40 && r.heap_after.allocations==42);
    assert(r.heap_before.frees==30 && r.heap_after.frees==32);
    assert(r.heap_after.live_bytes==64+(256u<<10));
    assert(!strcmp(opened,"/app0/sce_module/win32u.so") && open_mode==2);
    assert(!strcmp(symbol,"__wine_unix_lib_init") && !r.error[0]);

    /* Each failure stops at its own step. */
    reset();load_rc=(int32_t)0x80020002;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_LOAD && r.ntdll_handle==(int32_t)0x80020002 && !starts);
    reset();no_descriptor=1;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_DESCRIPTOR && !starts);
    reset();build_image(4);
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_EXPORTS && r.missing_exports==1u<<4 && !starts);
    reset();start_rc=-5;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_START && r.module_start_rc==-5 && !adopted[0]);
    reset();adopt_ok=0;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_ADOPT && !strcmp(r.error,"module not found"));
    reset();malloc_ok=0;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_HEAP && r.heap_after.failures==2 && !opened[0]);
    reset();open_ok=0;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_DLOPEN && !strcmp(r.error,"module not found"));
    reset();sym_ok=0;
    assert(run(&r)==-1 && r.step==PW_WINE_UNIX_STEP_DLSYM && !strcmp(pw_wine_unix_probe_step_name(r.step),"dlsym"));

    /* Bad arguments. */
    reset();
    const PwWineUnixProbeOps none={NULL,NULL};
    assert(pw_wine_unix_probe(&none,"/app0",&r)==-1 && r.step==PW_WINE_UNIX_STEP_LOAD && !loaded[0]);
    assert(pw_wine_unix_probe(&ops,NULL,&r)==-1 && pw_wine_unix_probe(&ops,"/app0",NULL)==-1);
    char long_dir[300];memset(long_dir,'a',sizeof(long_dir)-1);long_dir[sizeof(long_dir)-1]=0;
    assert(pw_wine_unix_probe(&ops,long_dir,&r)==-1 && r.step==PW_WINE_UNIX_STEP_LOAD && !loaded[0]);
    assert(!strcmp(pw_wine_unix_probe_step_name(99),"unknown"));
    return 0;
}
