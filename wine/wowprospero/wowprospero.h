/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Shared PE/Unix contract of the prospero-win WoW64 CPU backend. */
#ifndef WOWPROSPERO_H
#define WOWPROSPERO_H

#include "../../include/prospero_win.h"

enum pw_wow_reason
{
    PW_WOW_SYSCALL = 1,     /* guest EIP reached the syscall BOP */
    PW_WOW_UNIXCALL = 2,    /* guest EIP reached the Unix-call BOP */
    PW_WOW_FAULT = 3,       /* guest memory access or instruction fetch refused */
    PW_WOW_UNSUPPORTED = 4, /* DBT cannot translate the instruction at EIP */
    PW_WOW_X87_TRAP = 5,
    PW_WOW_ERROR = 6,
};

struct pw_wow_run_params
{
    UINT64 context;         /* I386_CONTEXT *, 64-bit address */
    UINT   teb32;
    UINT   bop;
    UINT   unix_bop;
    UINT   reason;          /* out */
    INT    status;          /* out: DBT status for FAULT/UNSUPPORTED/ERROR */
    UINT   fault_address;   /* out */
    UINT   fault_write;     /* out */
};

struct pw_wow_flush_params
{
    UINT64 address;
    UINT64 size;
};

/* A protection change that succeeded: every page of [address, address +
 * size) is committed and has the Win32 protection prot now. */
struct pw_wow_protect_params
{
    UINT64 address;
    UINT64 size;
    UINT   prot;
};

/* A missing source span is an invalid guest instruction fetch, rather than
 * an internal translator failure. Do not reuse a previous data-fault address
 * or write flag when delivering that access violation. */
static inline void pw_wow_report_error(struct pw_wow_run_params *params, int status,
                                      UINT pc, UINT fault_address, UINT fault_write)
{
    params->status = status;
    params->fault_address = status == PW_ERR_NOT_FOUND ? pc : fault_address;
    params->fault_write = status == PW_ERR_NOT_FOUND ? 0 : fault_write;
    if (status == PW_ERR_VM || status == PW_ERR_NOT_FOUND) params->reason = PW_WOW_FAULT;
    else if (status == PW_ERR_UNSUPPORTED) params->reason = PW_WOW_UNSUPPORTED;
    else if (status == PW_ERR_X87_TRAP) params->reason = PW_WOW_X87_TRAP;
    else params->reason = PW_WOW_ERROR;
}

/* New calls go last, so the existing ones keep their numbers. The PE side
 * and the Unix side always ship together. */
enum pw_wow_funcs
{
    pw_wow_process_init,
    pw_wow_run,
    pw_wow_flush,
    pw_wow_thread_term,
    pw_wow_dump,
    pw_wow_protect,
    pw_wow_funcs_count
};

#endif
