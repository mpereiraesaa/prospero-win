/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_HOSTEXEC_H
#define PW_X86_HOSTEXEC_H
/*
 * Host-executed fallback for single IA-32 instructions the translator does
 * not cover.
 *
 * The host is x86-64, so most non-control, non-stack i386 instructions have
 * an x86-64 encoding with identical semantics once three things are fixed:
 * an 0x67 prefix keeps address arithmetic, moffs and string pointers 32-bit;
 * the memory operand is replaced by [esi] or [edi] holding the effective
 * address computed here (including the guest FS base); and guest ESP never
 * appears as a register operand. The rewritten instruction runs in a small
 * stub that loads the guest GPRs, arithmetic flags, x87 (FSAVE image), SSE
 * registers and MXCSR, executes it and stores them back, preserving the host
 * FPU control word, MXCSR and callee-saved registers.
 *
 * Refused (PW_ERR_UNSUPPORTED): control transfers, implicit stack users,
 * instructions invalid in 64-bit mode, segment-register and privileged or I/O
 * forms, a guest 0x67 prefix, GS overrides and register uses of ESP. Those
 * stay with the translator or dedicated emulation.
 *
 * Stubs depend only on the instruction bytes, so they are cached per guest PC
 * and published once through the W^X VM backend.
 */
#include "pw_x86_block.h"
#include "../include/prospero_win_vm.h"

enum { PW_X86_HOSTEXEC_SLOTS = 4096, PW_X86_HOSTEXEC_STUB_MAX = 512 };

typedef struct PwX86HostExecSlot {
    uint32_t pc;
    uint8_t length, used, raw[15];
    uint32_t offset;       /* stub offset in the code region */
    uint8_t mem_form;      /* 1 when the stub expects an effective address */
} PwX86HostExecSlot;

typedef struct PwX86HostExec {
    const PwVmBackend *backend;
    PwVmRegion code;
    size_t cursor;
    uint64_t executed, compiled, refused;
    PwX86HostExecSlot slots[PW_X86_HOSTEXEC_SLOTS];
    unsigned initialized;
} PwX86HostExec;

/* Result of decoding one instruction for host execution. */
typedef struct PwX86HostExecPlan {
    uint8_t length;        /* guest instruction length */
    uint8_t mem_form;      /* has a ModRM memory operand */
    uint8_t address_reg;   /* host GPR carrying the effective address */
    uint8_t out_bytes;
    uint8_t out[20];       /* rewritten host instruction */
    uint32_t address;      /* effective address (valid when mem_form) */
} PwX86HostExecPlan;

int pw_x86_hostexec_init(PwX86HostExec *, const PwVmBackend *, size_t code_bytes);
int pw_x86_hostexec_reset(PwX86HostExec *);
int pw_x86_hostexec_destroy(PwX86HostExec *);
/* Decode and rewrite the instruction at `source` for the current guest state;
 * computes the effective address but executes nothing. */
int pw_x86_hostexec_plan(const PwX86State *, const uint8_t *source, size_t bytes,
                         PwX86HostExecPlan *);
/* Execute exactly one instruction at state->eip (bytes at `source`) and
 * advance EIP. Flags must already be canonical (no deferred flags). */
int pw_x86_hostexec_step(PwX86HostExec *, PwX86State *, const uint8_t *source,
                         size_t bytes);
#endif
