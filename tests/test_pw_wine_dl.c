/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../wine/ps5/pw_wine_dl.h"
#include "../wine/ps5/pw_wine_prx.h"
#include <assert.h>
#include <string.h>

static _Alignas(16) uint8_t image[2048];
static int starts,loads,unloads,unload_fail;
static char last_path[PW_WINE_DL_MAX_PATH];

static int module_start(size_t argc,const void *argv){(void)argc;(void)argv;starts++;return 0;}

static int32_t load_start(const char *path,size_t argc,const void *argv,uint32_t flags,
                          const void *option,int *result)
{
    (void)argc;(void)argv;(void)flags;(void)option;*result=0;strcpy(last_path,path);loads++;
    if(!strcmp(path,"/app0/win/lib/win32u.prx"))return 7;
    if(!strcmp(path,"/app0/sce_module/other.prx"))return 8;
    if(!strcmp(path,"/app0/win/lib/bare.prx"))return 9;
    return (int32_t)0x80020002;
}
static int module_info(int32_t handle,void *info)
{
    uint8_t *raw=info;uint64_t size;memcpy(&size,raw,8);assert(size==PW_PRX_MODULE_INFO_BYTES);
    /* Module 9 carries no descriptor at all. */
    uint64_t data=(uintptr_t)(handle==9?image+1024:image),code=(uintptr_t)module_start;
    uint32_t bytes=1024,rd=1,rx=5,count=2,code_bytes=64;
    memcpy(raw+0x108,&data,8);memcpy(raw+0x110,&bytes,4);memcpy(raw+0x114,&rd,4);
    memcpy(raw+0x118,&code,8);memcpy(raw+0x120,&code_bytes,4);memcpy(raw+0x124,&rx,4);
    memcpy(raw+0x148,&count,4);
    return 0;
}
static int stop_unload(int32_t handle,size_t argc,const void *argv,uint32_t flags,
                       const void *option,int *result)
{
    (void)handle;(void)argc;(void)argv;(void)flags;(void)option;*result=0;unloads++;
    return unload_fail?-1:0;
}

int main(void)
{
    /* The module image: names, a data export inside, and module_start. */
    char *n1=(char *)image+32,*n2=(char *)image+64;
    strcpy(n1,"__wine_unix_call_funcs");strcpy(n2,"module_start");
    PwPrxDescriptor *d=(PwPrxDescriptor *)(image+256);
    d->magic=PW_PRX_MAGIC;d->version=1;d->count=2;
    d->exports[0]=(PwPrxExport){n1,image+128};
    d->exports[1]=(PwPrxExport){n2,(const void *)(uintptr_t)module_start};
    const PwWineDlOps ops={load_start,module_info,stop_unload};
    pw_wine_dl_configure(&ops,"/app0/sce_module");

    /* dir/name.so loads dir/name.prx, starts it once and resolves names. */
    void *win32u=pw_wine_dl_open("/app0/win/lib/win32u.so");
    assert(win32u && starts==1 && !strcmp(last_path,"/app0/win/lib/win32u.prx"));
    assert(pw_wine_dl_sym(win32u,"__wine_unix_call_funcs")==image+128);
    assert(!pw_wine_dl_sym(win32u,"__wine_unix_call_wow64_funcs"));
    assert(!strcmp(pw_wine_dl_error(),"symbol not found") && !pw_wine_dl_error());
    /* The same path is reference counted, not reloaded or restarted. */
    assert(pw_wine_dl_open("/app0/win/lib/win32u.so")==win32u && loads==1 && starts==1);
    /* The module directory is the fallback for a missing sibling .prx. */
    void *other=pw_wine_dl_open("/usr/lib/wine/other.so");
    assert(other && other!=win32u && !strcmp(last_path,"/app0/sce_module/other.prx") && loads==3);
    /* The default handle searches every loaded module. */
    assert(pw_wine_dl_sym(PW_WINE_DL_DEFAULT,"__wine_unix_call_funcs")==image+128);
    assert(!pw_wine_dl_sym(PW_WINE_DL_DEFAULT,"wine_main_preload_info"));
    /* Missing modules and modules without a descriptor fail cleanly. */
    assert(!pw_wine_dl_open("/app0/win/lib/absent.so") && !strcmp(pw_wine_dl_error(),"module not found"));
    int before=unloads;
    assert(!pw_wine_dl_open("/app0/win/lib/bare.so") && unloads==before+1);
    assert(!strcmp(pw_wine_dl_error(),"module has no export descriptor"));
    assert(!pw_wine_dl_open(NULL) && !pw_wine_dl_open(""));
    /* Closing: the last reference unloads; a stale handle is refused. */
    before=unloads;
    assert(!pw_wine_dl_close(win32u) && unloads==before);
    assert(!pw_wine_dl_close(win32u) && unloads==before+1);
    assert(pw_wine_dl_close(win32u)==-1 && !strcmp(pw_wine_dl_error(),"invalid handle"));
    assert(!pw_wine_dl_sym(win32u,"__wine_unix_call_funcs"));
    assert(pw_wine_dl_sym(PW_WINE_DL_DEFAULT,"__wine_unix_call_funcs")==image+128);
    unload_fail=1;assert(pw_wine_dl_close(other)==-1 && !strcmp(pw_wine_dl_error(),"unload failed"));
    assert(!pw_wine_dl_sym(PW_WINE_DL_DEFAULT,"__wine_unix_call_funcs"));
    /* A reopened module starts again. */
    unload_fail=0;void *again=pw_wine_dl_open("/app0/win/lib/win32u.so");
    assert(again && starts==3);assert(!pw_wine_dl_close(again));
    return 0;
}
