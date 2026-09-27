/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_BLOCK_H
#define PW_X86_BLOCK_H
#include <stddef.h>
#include <stdint.h>
#include "pw_guest_fp.h"
#include "../include/prospero_win.h"

/* The gate declares the stack, the TEB, the PEB and its parameters page plus
 * each module's image and writable span, and then appends every NT allocation
 * the guest is handed while the same run is in flight. One table holds both
 * writers, so its bound is the sum of the two budgets - 8 modules * 2 ranges
 * + 4 process blocks + 16 NT regions - and not either budget on its own; the
 * gate is the only place both constants are visible and asserts the sum
 * there, together with the imm8 bound the generated guard encodes. Fields
 * after memory[] are addressed with disp32, so widening the table does not
 * move anything the emitter indexes. */
/* PW_X86_EXEC is carried so a region can remember the protection it was given
 * through NtProtectVirtualMemory and report it back as the old protection; the
 * access guard itself only ever asks for read and write. */
/* Host register ids are r8 + id. Id 3 (r11) is the emitter's scratch and is
 * never handed out; a block's own allocator uses ids 0..2 (r8-r10), and the
 * global assignment also uses the callee-saved r12-r15 (ids 4..7), which is
 * why generated code must be entered through pw_x86_run_block. */
enum { PW_X86_MEMORY_REGIONS=256, PW_X86_READ=1, PW_X86_WRITE=2, PW_X86_EXEC=4,
       PW_X86_MAX_HOST_REGS=8, PW_X86_LOCAL_HOST_REGS=3 };
typedef struct PwX86Memory {
    uint32_t low;
    uint64_t high; /* exclusive; can represent 4 GiB */
    unsigned permissions;
} PwX86Memory;

typedef struct PwX86RegContract {
    uint8_t resident_mask;  /* bitmask of guest GPRs (0..7) resident in host registers */
    uint8_t dirty_mask;     /* bitmask of resident guest GPRs modified */
    int8_t guest_to_host[8];/* guest GPR (0..7) -> host reg (0..PW_X86_MAX_HOST_REGS-1) or -1 */
    int8_t host_to_guest[PW_X86_MAX_HOST_REGS]; /* host reg ID -> guest GPR or -1 */
} PwX86RegContract;

static inline int pw_x86_contracts_match(const PwX86RegContract *a, const PwX86RegContract *b)
{
    if (a->resident_mask != b->resident_mask) return 0;
    for (int i = 0; i < 8; i++) {
        if (a->guest_to_host[i] != b->guest_to_host[i]) return 0;
    }
    return 1;
}

typedef struct PwX86DeferredFlags {
    uint32_t raw_flags; /* most recent native arithmetic flags, merged by known_mask */
    uint32_t known_mask;/* deferred subset of CF/PF/AF/ZF/SF/OF; zero means empty */
} PwX86DeferredFlags;

typedef struct PwX86State {
    uint32_t gpr[8]; /* eax ecx edx ebx esp ebp esi edi */
    uint32_t eip;
    uint32_t stack_low, stack_high; /* mapped RW range, high exclusive */
    uint32_t fs_base, fs_bytes; /* guest-owned RW thread region, not host FS */
    uint32_t eflags; /* guest flags; never installed as host control flags */
    unsigned memory_count;
    uint32_t chain_budget;       /* remaining blocks in current chain quantum */
    uint32_t step_retired;       /* cumulative instructions retired in current dispatch step */
    uint32_t step_transitions;   /* linked block-to-block transitions in current step */
    uintptr_t last_exit_slot;    /* address of link slot that triggered unlinked exit (or 0) */
    uint32_t reg_loads;          /* guest-state loads performed in step */
    uint32_t reg_stores;         /* guest-state stores performed in step */
    uint32_t reg_reconciliations;/* cross-block reconciliations in step */
    uint32_t reg_spills;         /* register spills performed in step */
    PwX86Memory memory[PW_X86_MEMORY_REGIONS]; /* live identity-mapped ranges */
    PwGuestFp fp;
    /* Keep lazy-flag state after the compact generated-code ABI above.  The
     * emitter addresses these fields with disp32, so adding observability
     * cannot silently move memory[] beyond a signed disp8. */
    PwX86DeferredFlags deferred_flags;    /* active deferred flags descriptor */
    /* Why the memory guard refused the last access, so a classified stop can
     * name the address instead of only its kind. */
    uint32_t fault_address, fault_width, fault_write;
} PwX86State;

uint32_t pw_x86_compute_canonical_flags(const PwX86DeferredFlags *df, uint32_t prev_eflags);
void pw_x86_materialize_flag_bits(PwX86State *state, uint32_t demand_mask);
void pw_x86_commit_canonical_flags(PwX86State *state);

typedef enum PwX86ExitKind {
    PW_X86_EXIT_NONE = 0,
    PW_X86_EXIT_DIRECT_JUMP,   /* Direct unconditional branch: jmp rel8/rel32, call rel32 */
    PW_X86_EXIT_CONDITIONAL,   /* Conditional branch: jcc rel8/rel32 */
    PW_X86_EXIT_DYNAMIC        /* Ret, indirect call/jump, trap, fault, max block length */
} PwX86ExitKind;

typedef struct PwX86ExitDesc {
    PwX86ExitKind kind;
    unsigned chainable;
    uint32_t target_pc;         /* taken or direct jump target guest PC */
    uint32_t fallthrough_pc;    /* not-taken target guest PC (if conditional) */
    size_t target_patch_offset; /* offset in emitted code of 64-bit slot pointer for target */
    size_t fallthrough_patch_offset; /* offset in emitted code of 64-bit slot pointer for fallthrough */
    size_t target_stub_offset;  /* offset in emitted code of unlinked exit stub for target */
    size_t fallthrough_stub_offset; /* offset in emitted code of unlinked exit stub for fallthrough */
    size_t target_reconcile_offset; /* offset in emitted code of reconciliation stub for target */
    size_t fallthrough_reconcile_offset; /* offset in emitted code of reconciliation stub for fallthrough */
    size_t target_reconcile_patch_offset; /* offset in code of canonical_code pointer for target */
    size_t fallthrough_reconcile_patch_offset; /* offset in code of canonical_code pointer for fallthrough */
} PwX86ExitDesc;

typedef struct PwX86Block {
    size_t source_bytes, code_bytes;
    uint32_t instructions;
    /* End offset of each guest instruction. This lets the dispatcher report
     * the precise retired prefix when a generated memory guard exits early. */
    uint16_t instruction_ends[32];
    size_t canonical_entry_offset;
    size_t chain_entry_offset;
    PwX86RegContract entry_contract;
    PwX86RegContract exit_contract;
    PwX86ExitDesc exit;
} PwX86Block;

/* Initial bounded DBT subset: push immediate/register/memory, pop register,
 * mov register/immediate, register/register or registered memory (ModRM/SIB),
 * MOV immediate/register or memory, register/memory ADD/OR/ADC/SBB/AND/SUB/XOR
 * with arithmetic flags,
 * NOT/NEG register/memory, LEAVE,
 * byte MOV/CMP/TEST (including high registers), register/memory INC/DEC,
 * immediate ALU 16/32-bit (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP),
 * CMP register/memory 32-bit, TEST 32-bit register/memory/immediate, MOVZX word,
 * short/near Jcc and register-byte SETcc using guest arithmetic flags,
 * LEA, absolute and FS moffs32/EAX loads/stores, nop,
 * direct/indirect near call/jump and ret/ret imm16. No copied 32-bit stack instructions. A successful
 * block is a SysV int(PwX86State*) function returning 0, or -1 on memory bounds.
 * Direct transfers update guest EIP and return to the dispatcher.
 * Stack range must be live RW identity-mapped guest memory below 4 GiB.
 * FS range must also be live RW guest memory; no host segment state is used.
 * Additional memory ranges must be live identity mappings with the declared
 * permissions. Generated code embeds a process-local memory-check helper.
 * Output is unexecutable scratch on failure; never publish failed output.
 * This is not an x86 engine yet: unsupported instructions stop translation. */
int pw_x86_translate(const uint8_t *source, size_t bytes, uint32_t guest_pc,
                     uint8_t *output, size_t capacity, PwX86Block *block);
int pw_x86_translate_ext(const uint8_t *source, size_t bytes, uint32_t guest_pc,
                         uint8_t *output, size_t capacity, PwX86Block *block,
                         unsigned residency_enabled, unsigned lazy_flags_enabled);

/*
 * Indirect targets: a direct-mapped table from guest PC to the host entry
 * that runs it with every guest register in PwX86State. A ret, an indirect
 * call or an indirect jump translated with a table looks its target up there
 * and jumps straight to it, spending one unit of the chain budget as a linked
 * exit does; a miss, an empty slot or an exhausted budget returns to the
 * dispatcher as before. The table's owner fills it and clears it whenever the
 * code it points into is discarded.
 */
typedef struct PwX86IndirectTarget {
    uint32_t guest_pc;
    uint32_t reserved;
    const void *host_code;      /* NULL: empty */
} PwX86IndirectTarget;

static inline uint32_t pw_x86_indirect_slot(uint32_t guest_pc, uint32_t mask)
{
    return (guest_pc ^ (guest_pc >> 12)) & mask;
}

typedef struct PwX86TranslateOptions {
    unsigned residency_enabled;
    unsigned lazy_flags_enabled;
    const PwX86IndirectTarget *indirect_targets;   /* NULL: dynamic exits return */
    uint32_t indirect_mask;                        /* slots - 1, a power of two minus one */
    /* A flat guest address space: when flat_high > flat_low (at least 16
     * bytes apart), the caller promises the stack range and a single RW
     * region are both exactly [flat_low, flat_high), and every guest access
     * inside it is checked with one compare. 0/0 uses the region table. */
    uint32_t flat_low, flat_high;
    /* Leave out the statistics counters (step_retired, step_transitions and
     * the reg_* counts): a step's report then counts no retired instructions
     * and the engine's transition and register totals stay zero. */
    unsigned no_counters;
    /* The re-encoder's own indirect targets (pw_x86_reencode.h): 65536
     * slots indexed by the low 16 bits of the guest PC, pointing at chain
     * entries, so a return between re-encoded blocks stays pinned. NULL:
     * dynamic exits use indirect_targets only. */
    const PwX86IndirectTarget *chain_targets;
    /* Guest GPRs (bit n = gpr n) held in the same host register by every
     * block without a helper call, so linked blocks pass them on without a
     * store or load; at most seven. 0 keeps the per-block allocator. */
    uint8_t global_resident;
} PwX86TranslateOptions;

/* The host register the global assignment gives guest GPR gpr under mask,
 * or -1: the set bits take ids 0, 1, 2, 4, 5, 6, 7 in order. */
static inline int pw_x86_global_host(uint8_t mask, unsigned gpr)
{
    static const int8_t ids[7] = { 0, 1, 2, 4, 5, 6, 7 };
    unsigned slot = 0;

    if (gpr >= 8 || !(mask & (1u << gpr))) return -1;
    for (unsigned g = 0; g < gpr; g++) slot += (mask >> g) & 1u;
    return slot < 7 ? ids[slot] : -1;
}

/* Run generated code at entry with state: the SysV call a block expects,
 * with rbx, rbp and r12-r15 saved for the caller, since resident guest
 * values may live in them. Returns what the block returns. */
int pw_x86_run_block(PwX86State *state, const void *entry);

int pw_x86_translate_opts(const uint8_t *source, size_t bytes, uint32_t guest_pc,
                          uint8_t *output, size_t capacity, PwX86Block *block,
                          const PwX86TranslateOptions *options);
#endif
