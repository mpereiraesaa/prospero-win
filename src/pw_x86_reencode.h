/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_REENCODE_H
#define PW_X86_REENCODE_H
/*
 * Same-ISA re-encoding of i386 blocks for an x86-64 host.
 *
 * Guest and host are both x86, so most guest instructions run as themselves:
 * the eight guest GPRs stay pinned in host registers for the whole of a
 * linked chain (eax ecx edx ebx ebp esi in their own registers, esp in r12,
 * edi in r13, PwX86State in rdi), and the guest's arithmetic flags live in
 * the host RFLAGS, saved to PwX86State only when the chain returns to C.
 * Register forms are copied with the encoding adjusted for r12/r13; a memory
 * operand is addressed through r11 after the flat guard (the guest range is
 * identity-mapped below 4 GiB, so the guest address is the host address).
 * The glue between instructions is flag-free (mov, lea, xchg, jrcxz), and
 * the guard, which compares, keeps the flags around itself whenever a later
 * instruction can read them. Stack instructions, calls and returns, which
 * use the host stack natively, are translated onto r12.
 *
 * Only flat address spaces (PwX86TranslateOptions.flat_low/high) without the
 * statistics counters are re-encoded. A block ends before the first
 * instruction this backend does not take, which the older emitter then
 * translates; the two meet through the canonical entries (the contracts
 * never match), so either can follow the other.
 *
 * Differences from the older emitter, both deliberate:
 * - a fault reports the incoming arithmetic flags exactly only when a later
 *   instruction reads them; when every flag is redefined before any read,
 *   the reported flags are whatever the guard left, which re-execution of
 *   the faulting instruction never observes;
 * - step_retired is not counted (as with no_counters).
 */
#include "pw_x86_block.h"

/* The contract every re-encoded block has: all eight guest GPRs resident,
 * with guest_to_host holding 16 + the host register number. */
enum { PW_X86_REENCODE_HOST_BASE = 16 };

/* Translate the block at pc. PW_OK with a block of at least one
 * instruction; PW_ERR_UNSUPPORTED when the options are not flat or keep
 * counters, or when the first instruction is not one this backend takes.
 * Other errors as pw_x86_translate_opts. */
int pw_x86_reencode(const uint8_t *source, size_t bytes, uint32_t guest_pc,
                    uint8_t *output, size_t capacity, PwX86Block *block,
                    const PwX86TranslateOptions *options);
#endif
