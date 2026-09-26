/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Shared PE/Unix contract of the prospero-win WoW64 CPU backend. */
#ifndef WOWPROSPERO_H
#define WOWPROSPERO_H

enum pw_wow_reason
{
    PW_WOW_SYSCALL = 1,     /* guest EIP reached the syscall BOP */
    PW_WOW_UNIXCALL = 2,    /* guest EIP reached the Unix-call BOP */
    PW_WOW_FAULT = 3,       /* DBT memory guard refused an access */
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

enum pw_wow_funcs
{
    pw_wow_process_init,
    pw_wow_run,
    pw_wow_flush,
    pw_wow_thread_term,
    pw_wow_dump,
    pw_wow_funcs_count
};

#endif
