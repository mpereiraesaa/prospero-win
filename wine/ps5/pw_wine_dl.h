/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_DL_H
#define PW_WINE_DL_H
#include <stddef.h>
#include <stdint.h>

/* dlopen-style loading of Wine's Unix modules as PRX on PS5. A request for
 * "dir/name.so" loads "dir/name.prx", or "<module dir>/name.prx" when a
 * module directory is set. A bare "name.so" is looked up in the module
 * directory, then beside each module already loaded or adopted, so win32u's
 * dlopen("libfreetype.so") finds libfreetype.prx beside ntdll.prx. The
 * module's PRXDESC1 descriptor supplies every symbol, and its module_start,
 * when exported, is called once after load because the firmware does not. Loads are reference counted by path.
 * The kernel calls are injected so the logic is host-testable. */
enum { PW_WINE_DL_MAX_MODULES=32,PW_WINE_DL_MAX_PATH=512,PW_WINE_DL_REPORT=1024 };
typedef struct PwWineDlOps {
    int32_t (*load_start)(const char *path,size_t argc,const void *argv,uint32_t flags,
                          const void *option,int *result);
    int (*module_info)(int32_t handle,void *info);
    int (*stop_unload)(int32_t handle,size_t argc,const void *argv,uint32_t flags,
                       const void *option,int *result);
    /* Optional: called with one line when an open fails, naming the module,
     * every path tried and why, so the console log says what went wrong. */
    void (*report)(const char *line);
} PwWineDlOps;
#define PW_WINE_DL_DEFAULT ((void *)0)
typedef struct PwWineDlInfo {
    const char *path;          /* the path the module was opened or adopted as */
    const void *base;          /* its first segment */
} PwWineDlInfo;

void pw_wine_dl_configure(const PwWineDlOps *ops,const char *module_dir);
void *pw_wine_dl_open(const char *path);
/* handle PW_WINE_DL_DEFAULT searches every loaded module in load order. */
void *pw_wine_dl_sym(void *handle,const char *name);
int pw_wine_dl_close(void *handle);
/* Registers a module the title loaded itself (ntdll.prx, for instance) so
 * that dladdr can name it; returns a handle like pw_wine_dl_open. */
void *pw_wine_dl_adopt(const char *path,int32_t module_handle);
/* The module whose segments contain address, as dladdr reports it. */
int pw_wine_dl_addr(const void *address,PwWineDlInfo *info);
/* The last failure of this thread, cleared once read, as dlerror does. */
const char *pw_wine_dl_error(void);

#endif
