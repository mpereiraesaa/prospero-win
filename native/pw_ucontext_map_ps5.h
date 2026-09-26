/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_UCONTEXT_MAP_PS5_H
#define PW_UCONTEXT_MAP_PS5_H
#include <stdint.h>

/* Registers located in the signal ucontext, in this order. */
enum {
    PW_UC_RAX, PW_UC_RBX, PW_UC_RCX, PW_UC_RDX, PW_UC_RSI, PW_UC_RDI, PW_UC_RBP,
    PW_UC_R8, PW_UC_R9, PW_UC_R10, PW_UC_R11, PW_UC_R12, PW_UC_R13, PW_UC_R14,
    PW_UC_R15, PW_UC_RSP, PW_UC_RIP, PW_UC_COUNT
};

typedef struct PwUcontextMap {
    int rc;                         /* 0 once the handler ran */
    int offset[PW_UC_COUNT];        /* byte offset in ucontext, -1 if not found */
    int duplicates;                 /* sentinels found at more than one offset */
    int header_rip, header_rsp, header_rax; /* offsets the SDK header claims */
    int header_size;                /* sizeof(ucontext_t) per the SDK header */
    int scanned;                    /* bytes searched */
    int signal;                     /* SIGSEGV or SIGBUS for the non-canonical load */
} PwUcontextMap;

int pw_ucontext_map_ps5(PwUcontextMap *map);

/* User-mode FSGSBASE instructions (CR4.FSGSBASE): 1 = executed, 0 = SIGILL. */
typedef struct PwFsGsBaseProbe {
    int rdfsbase, rdgsbase, wrgsbase;
    uint64_t fs_value, gs_value;
} PwFsGsBaseProbe;
void pw_fsgsbase_ps5_probe(PwFsGsBaseProbe *probe);
#endif
