/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_wine_unix_probe.h"
#include "pw_wine_prx.h"
#include <stdio.h>
#include <string.h>

/* What the title resolves from ntdll.prx's descriptor, in bit order. */
static const char *const exports[]={
    "module_start","pw_wine_dl_adopt","dlopen","dlsym","dlerror",
    "pw_wine_heap_stats","pw_wine_heap_malloc","pw_wine_heap_free",
};
enum { EXPORT_COUNT=sizeof(exports)/sizeof(exports[0]), RTLD_NOW_FLAG=2 };

typedef int (*StartFn)(size_t,const void *);
typedef void *(*AdoptFn)(const char *,int32_t);
typedef void *(*OpenFn)(const char *,int);
typedef void *(*SymFn)(void *,const char *);
typedef char *(*ErrorFn)(void);
typedef void (*StatsFn)(PwWineHeapStats *);
typedef void *(*MallocFn)(size_t);
typedef void (*FreeFn)(void *);

const char *pw_wine_unix_probe_step_name(int step)
{
    static const char *const names[]={"none","load","descriptor","exports","start","adopt",
                                      "heap","dlopen","dlsym","done"};
    return step>=0 && step<=PW_WINE_UNIX_STEP_DONE?names[step]:"unknown";
}
static void enter(const PwWineUnixProbeOps *ops,PwWineUnixProbeReport *r,int step)
{
    r->step=step;
    if(ops->on_step)ops->on_step(step);
}
static int fail(PwWineUnixProbeReport *r,int step,ErrorFn error)
{
    const char *text=error?error():NULL;
    r->step=step;
    snprintf(r->error,sizeof(r->error),"%s",text?text:"");
    return -1;
}

int pw_wine_unix_probe(const PwWineUnixProbeOps *ops,const char *dir,PwWineUnixProbeReport *r)
{
    char ntdll[256],win32u[256];
    const void *found[EXPORT_COUNT];
    if(!r)return -1;
    memset(r,0,sizeof(*r));
    if(!ops || !ops->load_start || !ops->module_info || !dir ||
       snprintf(ntdll,sizeof(ntdll),"%s/ntdll.prx",dir)>=(int)sizeof(ntdll) ||
       snprintf(win32u,sizeof(win32u),"%s/win32u.so",dir)>=(int)sizeof(win32u))
        return fail(r,PW_WINE_UNIX_STEP_LOAD,NULL);

    enter(ops,r,PW_WINE_UNIX_STEP_LOAD);
    r->ntdll_handle=ops->load_start(ntdll,0,NULL,0,NULL,&r->start_result);
    if(r->ntdll_handle<0)return fail(r,PW_WINE_UNIX_STEP_LOAD,NULL);

    _Alignas(8) uint8_t info[PW_PRX_MODULE_INFO_BYTES];
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
    uint32_t count=0;
    const PwPrxDescriptor *descriptor=NULL;
    uint64_t size=PW_PRX_MODULE_INFO_BYTES;
    enter(ops,r,PW_WINE_UNIX_STEP_DESCRIPTOR);
    memset(info,0,sizeof(info));memcpy(info,&size,sizeof(size));
    if(ops->module_info(r->ntdll_handle,info)<0 ||
       pw_prx_parse_module_info(info,NULL,segments,&count)!=PW_PRX_OK ||
       pw_prx_find_descriptor(segments,count,&descriptor)!=PW_PRX_OK)
        return fail(r,PW_WINE_UNIX_STEP_DESCRIPTOR,NULL);

    enter(ops,r,PW_WINE_UNIX_STEP_EXPORTS);
    for(uint32_t i=0;i<EXPORT_COUNT;i++)
        if(!(found[i]=pw_prx_lookup(descriptor,exports[i])))r->missing_exports|=1u<<i;
    if(r->missing_exports)return fail(r,PW_WINE_UNIX_STEP_EXPORTS,NULL);
    ErrorFn error=(ErrorFn)(uintptr_t)found[4];

    /* Idempotent: the loader's entry call or .init_array may have run it. */
    enter(ops,r,PW_WINE_UNIX_STEP_START);
    r->module_start_rc=((StartFn)(uintptr_t)found[0])(0,NULL);
    if(r->module_start_rc)return fail(r,PW_WINE_UNIX_STEP_START,error);

    enter(ops,r,PW_WINE_UNIX_STEP_ADOPT);
    if(!((AdoptFn)(uintptr_t)found[1])(ntdll,r->ntdll_handle))
        return fail(r,PW_WINE_UNIX_STEP_ADOPT,error);

    /* One class allocation and one large mapping, both returned. */
    enter(ops,r,PW_WINE_UNIX_STEP_HEAP);
    StatsFn stats=(StatsFn)(uintptr_t)found[5];
    MallocFn heap_malloc=(MallocFn)(uintptr_t)found[6];
    FreeFn heap_free=(FreeFn)(uintptr_t)found[7];
    stats(&r->heap_before);
    uint8_t *small=heap_malloc(64),*large=heap_malloc((size_t)256<<10);
    if(small)memset(small,0x5a,64);
    if(large)memset(large,0xa5,(size_t)256<<10);
    int filled=small && large && small[63]==0x5a && large[((size_t)256<<10)-1]==0xa5;
    heap_free(small);heap_free(large);
    stats(&r->heap_after);
    if(!filled || r->heap_after.allocations<r->heap_before.allocations+2 ||
       r->heap_after.frees<r->heap_before.frees+2 || r->heap_after.failures!=r->heap_before.failures)
        return fail(r,PW_WINE_UNIX_STEP_HEAP,NULL);

    /* win32u through ntdll: dependency loaded first, so its imports bind. */
    enter(ops,r,PW_WINE_UNIX_STEP_DLOPEN);
    void *module=((OpenFn)(uintptr_t)found[2])(win32u,RTLD_NOW_FLAG);
    if(!module)return fail(r,PW_WINE_UNIX_STEP_DLOPEN,error);
    enter(ops,r,PW_WINE_UNIX_STEP_DLSYM);
    if(!((SymFn)(uintptr_t)found[3])(module,"__wine_unix_lib_init"))
        return fail(r,PW_WINE_UNIX_STEP_DLSYM,error);
    enter(ops,r,PW_WINE_UNIX_STEP_DONE);
    return 0;
}
