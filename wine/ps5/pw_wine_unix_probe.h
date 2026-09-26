/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_UNIX_PROBE_H
#define PW_WINE_UNIX_PROBE_H
#include "pw_wine_heap.h"
#include <stddef.h>
#include <stdint.h>

/* Loads Wine's Unix side the way the title will, and records each step:
 * ntdll.prx through the firmware loader, its export descriptor, its
 * module_start, pw_wine_dl_adopt, a small and a large allocation through
 * its heap, then win32u through ntdll's own dlopen and __wine_unix_lib_init
 * through its dlsym. Nothing Wine-level is initialised or called. The
 * firmware calls are injected, so the sequence is host-testable. */
typedef struct PwWineUnixProbeOps {
    int32_t (*load_start)(const char *path,size_t argc,const void *argv,uint32_t flags,
                          const void *option,int *result);
    int (*module_info)(int32_t handle,void *info);
    /* Optional: called before each step, so a fault is attributable. */
    void (*on_step)(int step);
} PwWineUnixProbeOps;

enum {
    PW_WINE_UNIX_STEP_LOAD=1,PW_WINE_UNIX_STEP_DESCRIPTOR,PW_WINE_UNIX_STEP_EXPORTS,
    PW_WINE_UNIX_STEP_START,PW_WINE_UNIX_STEP_ADOPT,PW_WINE_UNIX_STEP_HEAP,
    PW_WINE_UNIX_STEP_DLOPEN,PW_WINE_UNIX_STEP_DLSYM,PW_WINE_UNIX_STEP_DONE,
    PW_WINE_UNIX_ERROR_BYTES=96
};

typedef struct PwWineUnixProbeReport {
    int step;                  /* the first step that failed, or DONE */
    int32_t ntdll_handle;      /* the loader's handle or error */
    int start_result;          /* the loader's module_start result */
    int module_start_rc;       /* the descriptor's module_start */
    uint32_t missing_exports;  /* bit i: ntdll export i of the probe's list */
    PwWineHeapStats heap_before,heap_after;
    char error[PW_WINE_UNIX_ERROR_BYTES]; /* ntdll's dlerror at the failing step */
} PwWineUnixProbeReport;

/* dir holds ntdll.prx and win32u.prx. Returns 0 when every step passed. */
int pw_wine_unix_probe(const PwWineUnixProbeOps *ops,const char *dir,PwWineUnixProbeReport *report);
const char *pw_wine_unix_probe_step_name(int step);

#endif
