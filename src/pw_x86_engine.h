/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_ENGINE_H
#define PW_X86_ENGINE_H
#include "pw_x86_cache.h"
#include "../include/prospero_win_vm.h"

enum { PW_X86_ENGINE_MAX_SOURCE=15*32, PW_X86_ENGINE_MAX_CODE=16384,
       PW_X86_ENGINE_INDIRECT_SLOTS=8192 };

/* Return one immutable executable source span beginning at guest_pc. The span
 * remains alive and unchanged for the engine generation. */
typedef int (*PwX86SourceView)(void *opaque,uint32_t guest_pc,
                               const uint8_t **source,size_t *bytes);

typedef struct PwX86StepReport {
    uint32_t guest_pc;
    uint32_t instructions,retired;
    size_t source_bytes,code_bytes;
    unsigned cache_hit;
} PwX86StepReport;

typedef struct PwX86Engine {
    const PwVmBackend *backend;
    PwVmRegion code;
    PwX86Cache cache;
    PwX86SourceView source_view;
    void *source_opaque;
    uint64_t dispatches,retired_instructions,compiles;
    uint64_t protection_calls,protection_bytes;
    uint64_t attempted_links, successful_links;
    uint64_t linked_transitions, dispatcher_transitions;
    uint64_t unlinks, safepoint_returns;
    uint64_t reg_loads, reg_stores;
    uint64_t reg_reconciliations, reg_spills;
    uint64_t flags_safepoint_commits;
    /* The block the last fault happened in, and the contract it was entered
     * with: a fault that names its block and its register residency is the
     * difference between a guest bug and a translation bug. */
    uint32_t fault_block_pc;
    uint32_t fault_block_instructions;
    uint8_t fault_block_resident_mask;
    int8_t fault_block_map[8];
    uint8_t fault_block_code[1024];
    uint32_t fault_block_code_bytes;
    uint32_t quantum;
    unsigned chaining_enabled;
    unsigned residency_enabled;
    unsigned lazy_flags_enabled;
    /* Indirect targets (pw_x86_block.h), reserved when first enabled. */
    unsigned indirect_enabled;
    PwVmRegion indirect;
    PwX86IndirectTarget *indirect_targets;
    /* The flat guest range (PwX86TranslateOptions.flat_low/high), or 0/0. */
    uint32_t flat_low, flat_high;
    unsigned no_counters;
    /* PwX86TranslateOptions.global_resident for blocks translated from now on. */
    uint8_t global_resident;
    unsigned sealed,failed,initialized;
} PwX86Engine;

enum { PW_X86_ENGINE_DEFAULT_QUANTUM = 64 };

/* The engine owns its code region but not entries or source memory. It is a
 * single-dispatcher object: reset/destroy require no executing block. Code
 * publication changes protection only on pages touched by the new block. */
int pw_x86_engine_init(PwX86Engine *,const PwVmBackend *,
                       PwX86CacheEntry *,uint32_t,size_t,uint32_t,
                       PwX86SourceView,void *);
int pw_x86_engine_set_quantum(PwX86Engine *, uint32_t);
int pw_x86_engine_set_chaining(PwX86Engine *, unsigned);
int pw_x86_engine_set_residency(PwX86Engine *, unsigned);
int pw_x86_engine_set_lazy_flags(PwX86Engine *, unsigned);
/* Let rets and indirect calls/jumps in blocks translated from now on enter
 * their target without the dispatcher, through a table the dispatcher fills
 * and every reset clears. It needs chaining: the lookup spends the chain
 * budget, which is one without it. */
int pw_x86_engine_set_indirect(PwX86Engine *, unsigned);
/* Hold the guest GPRs in mask (at most seven) in the same host registers in
 * every block translated from now on (PwX86TranslateOptions.global_resident);
 * 0 returns to the per-block allocator. It needs residency enabled. */
int pw_x86_engine_set_global_resident(PwX86Engine *, uint8_t mask);
/* Translate from now on for a flat guest address space [low, high): the
 * state's stack range and its single RW memory region must both be exactly
 * that range. The guard is then one compare per access; accesses outside it
 * still go through the region table. low == high turns it off. */
int pw_x86_engine_set_flat_memory(PwX86Engine *, uint32_t low, uint32_t high);
/* Leave the statistics counters out of blocks translated from now on (they
 * are on by default): a step then reports no retired instructions, and the
 * transition and register totals stay zero. The chain budget, the link
 * slots and every guest-visible effect are unchanged. */
int pw_x86_engine_set_counters(PwX86Engine *, unsigned enabled);
int pw_x86_engine_step(PwX86Engine *,PwX86State *,PwX86StepReport *);
int pw_x86_engine_reset(PwX86Engine *,uint32_t);
int pw_x86_engine_destroy(PwX86Engine *);
#endif
