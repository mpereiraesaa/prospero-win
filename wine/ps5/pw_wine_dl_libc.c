/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* dlfcn for Wine's Unix side on PS5. ntdll.so links this file, so its own
 * dlopen of a unix library (and win32u's, through the ntdll.so import) loads
 * a PRX through pw_wine_dl instead of the firmware's dynamic linker, which
 * cannot resolve an application module's exports. dlopen(NULL) and the
 * RTLD_* pseudo-handles search every loaded module. */
#include "pw_wine_dl.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>

extern int32_t sceKernelLoadStartModule(const char *,size_t,const void *,uint32_t,const void *,int *);
extern int sceKernelGetModuleInfo(int32_t,void *);
extern int sceKernelStopUnloadModule(int32_t,size_t,const void *,uint32_t,const void *,int *);

static pthread_once_t once=PTHREAD_ONCE_INIT;
static int self;

/* WINE_PRX_DIR names the fallback directory for modules that are not staged
 * beside the path Wine asks for; the title's sce_module is the default. */
static void configure(void)
{
    const PwWineDlOps ops={sceKernelLoadStartModule,sceKernelGetModuleInfo,sceKernelStopUnloadModule};
    const char *dir=getenv("WINE_PRX_DIR");
    pw_wine_dl_configure(&ops,dir && *dir?dir:"/app0/sce_module");
}
static void *resolve_handle(void *handle)
{
    return handle==&self || handle==RTLD_DEFAULT || handle==RTLD_NEXT || handle==RTLD_SELF?
        PW_WINE_DL_DEFAULT:handle;
}

void *dlopen(const char *path,int mode)
{
    (void)mode;pthread_once(&once,configure);
    return path?pw_wine_dl_open(path):&self;
}
void *dlsym(void *__restrict handle,const char *__restrict name)
{
    pthread_once(&once,configure);
    return pw_wine_dl_sym(resolve_handle(handle),name);
}
int dlclose(void *handle)
{
    return handle==&self?0:pw_wine_dl_close(handle);
}
char *dlerror(void)
{
    return (char *)pw_wine_dl_error();
}
