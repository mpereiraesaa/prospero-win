/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_WINE_PLATFORM_PS5_H
#define PW_WINE_PLATFORM_PS5_H
/*
 * Platform facts Wine's native side depends on, measured on the console.
 *
 * Running Wine's x86_64 PE modules (ntdll, wow64, wow64win, the CPU backend)
 * natively needs a per-thread GS base for the TEB; ntdll.so needs recoverable
 * SIGSEGV delivery on an alternate stack; an in-process wineserver needs
 * socketpair with SCM_RIGHTS and kqueue; Wine's allocators need more heap
 * than the title's libc malloc may offer. Each probe restores what it
 * changed and reports errno instead of aborting.
 */
#include <stdint.h>

typedef struct PwWinePlatformReport {
    int gsbase_set_rc, gsbase_errno;     /* sysarch(AMD64_SET_GSBASE) */
    int gsbase_read_ok;                  /* %gs:0x30 read back the fake TEB */
    int gsbase_thread_ok;                /* a second thread kept its own base */
    int sigsegv_rc, sigsegv_recovered;   /* SA_ONSTACK handler resumed */
    int sigsegv_on_altstack;
    int sigsegv_rip_edit_ok;             /* editing mc_rip resumed past the fault */
    int sigsegv_hits;                    /* handler entries (capped) */
    int sigsegv_rip_offset;              /* measured offset of RIP in ucontext, -1 if absent */
    int sigsegv_header_offset;           /* offsetof(ucontext_t, uc_mcontext.mc_rip) per SDK */
    int sigsegv_measured_edit_ok;        /* editing the measured slot resumed */
    int socketpair_rc, scm_rights_ok, socket_errno;
    int kqueue_rc, kevent_ready, kqueue_errno;
    uint64_t malloc_ceiling_bytes;       /* 1 MiB steps, capped at the probe limit */
    uint64_t malloc_limit_bytes;
} PwWinePlatformReport;

/* Individual steps, so the caller can log between them: a step that kills
 * the process still leaves the previous results in the log. */
void pw_wine_platform_ps5_sockets(PwWinePlatformReport *report);
void pw_wine_platform_ps5_sigsegv(PwWinePlatformReport *report);
void pw_wine_platform_ps5_malloc(PwWinePlatformReport *report);
/* Reads the current GS base (rc, errno, value) without changing it. */
int pw_wine_platform_ps5_gsbase_get(uint64_t *value, int *os_errno);
/* Rewrites the current GS base with itself (rc, errno). */
int pw_wine_platform_ps5_gsbase_same(int *os_errno);
void pw_wine_platform_ps5_gsbase(PwWinePlatformReport *report);
#endif
