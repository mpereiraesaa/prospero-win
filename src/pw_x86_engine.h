/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_ENGINE_H
#define PW_X86_ENGINE_H
#include "pw_x86_cache.h"
#include "../include/prospero_win_vm.h"

enum { PW_X86_ENGINE_MAX_SOURCE=15*32, PW_X86_ENGINE_MAX_CODE=16384,
       PW_X86_ENGINE_INDIRECT_SLOTS=8192, PW_X86_ENGINE_FAULT_GRANULE=256,
       PW_X86_ENGINE_CALL_STACK_GUARD=0x10000 };

/* Return one immutable executable source span beginning at guest_pc. The span
 * remains alive and unchanged for the engine generation. */
typedef int (*PwX86SourceView)(void *opaque,uint32_t guest_pc,
                               const uint8_t **source,size_t *bytes);
/* Optional owner-thread CPU clock in nanoseconds; zero means unavailable. */
typedef uint64_t (*PwX86ExecutionClock)(void *opaque);

typedef struct PwX86StepReport {
    uint32_t guest_pc;
    uint32_t instructions,retired;
    size_t source_bytes,code_bytes;
    unsigned cache_hit;
} PwX86StepReport;

/* Bounded, owner-thread sample counts. No pointers survive cache resets. */
enum { PW_X86_HOTSPOT_SLOTS = 4096 };
typedef struct PwX86Hotspot {
    uint32_t guest_pc;
    uint64_t samples, entry, body, exit, emitted;
} PwX86Hotspot;
typedef struct PwX86HotspotProfile {
    PwX86Hotspot slots[PW_X86_HOTSPOT_SLOTS];
    uint64_t samples, outside, stubs, overflow;
} PwX86HotspotProfile;

typedef struct PwX86Engine {
    const PwVmBackend *backend;
    PwVmRegion code;
    PwX86Cache cache;
    PwX86SourceView source_view;
    void *source_opaque;
    uint64_t dispatches,retired_instructions,compiles;
    PwX86ExecutionClock execution_clock;
    void *execution_clock_opaque;
    uint64_t execution_ns, execution_calls, execution_samples, execution_clock_errors;
    uint32_t execution_stride, execution_random;
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
    /* Try the same-ISA re-encoder (pw_x86_reencode.h) first, with its
     * chain-entry targets (reserved when first enabled). */
    unsigned reencode_enabled;
    PwVmRegion chain;
    PwX86IndirectTarget *chain_targets;
    uint64_t reencoded_blocks;
    /* PwX86TranslateOptions.fault_markers for blocks translated from now on,
     * and, to find the block a host fault is in, the first block that
     * overlaps each PW_X86_ENGINE_FAULT_GRANULE bytes of the arena (index +
     * 1; blocks follow each other through arena_next). */
    unsigned fault_markers;
    unsigned unbounded_chains;  /* PwX86TranslateOptions.unbounded_chains */
    unsigned superblocks;       /* PwX86TranslateOptions.superblocks */
    unsigned native_fp;         /* PwX86TranslateOptions.native_fp */
    uint8_t fxsave_image[512 + 15];  /* the guest's FP state while a block runs */
    /* The image above, not PwX86State.fp, holds the guest's x87/SSE state
     * (pw_x86_engine_fp_sync). */
    unsigned fp_image_live;
    /* PwX86TranslateOptions.call_stack: the memory the caller gave, its top
     * (PwX86State.call_stack_top), and where the return stub ends. */
    uint8_t *call_stack_base;
    size_t call_stack_bytes;
    uintptr_t call_stack_top;
    size_t return_stub_bytes;
    PwVmRegion block_map_region;
    uint32_t *block_map;
    uint32_t last_published;
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
/* Translate blocks from now on with the same-ISA re-encoder where it takes
 * them (pw_x86_reencode.h: a flat range and no counters), and with the
 * emitter elsewhere. */
int pw_x86_engine_set_reencode(PwX86Engine *, unsigned enabled);
/* Translate from now on for a flat guest address space [low, high): the
 * state's stack range and its single RW memory region must both be exactly
 * that range. The guard is then one compare per access; accesses outside it
 * still go through the region table. low == high turns it off. */
int pw_x86_engine_set_flat_memory(PwX86Engine *, uint32_t low, uint32_t high);
/* Re-encoded blocks translated from now on mark their accesses instead of
 * checking them (PwX86TranslateOptions.fault_markers); the caller then sends
 * every host fault in the engine's code region to
 * pw_x86_engine_fault_redirect. */
int pw_x86_engine_set_fault_markers(PwX86Engine *, unsigned enabled);
int pw_x86_engine_set_unbounded_chains(PwX86Engine *, unsigned enabled);
/* PwX86TranslateOptions.superblocks for blocks translated from now on: the
 * caller's code memory must stay writable while it runs. */
int pw_x86_engine_set_superblocks(PwX86Engine *, unsigned enabled);
/* PwX86TranslateOptions.native_fp, before any block is translated. */
int pw_x86_engine_set_native_fp(PwX86Engine *, unsigned enabled);
/* With native_fp, re-encoded blocks run on the guest's x87/SSE state as an
 * FXSAVE image in the host FPU. Converting it to and from PwX86State.fp
 * around every step cost an OpenGL game a fifth of its time, so the image
 * stays the guest's state from one step to the next, and the caller's
 * crossings copy it instead of converting it. PwX86State.fp is brought up to
 * date only for the code that reads it: emitter blocks, the host fallback,
 * and the caller, through pw_x86_engine_fp_sync. */
void pw_x86_engine_fp_sync(PwX86Engine *, PwX86State *);
/* The guest's state from an FXSAVE image (a thread context's
 * ExtendedRegisters), and back into one. */
void pw_x86_engine_fp_load(PwX86Engine *, PwX86State *, const uint8_t image[512]);
void pw_x86_engine_fp_store(PwX86Engine *, const PwX86State *, uint8_t image[512]);
/* Run re-encoded calls and returns on a call stack in [base, base+bytes)
 * (PwX86TranslateOptions.call_stack), or stop with base NULL. Needs the
 * re-encoder, the indirect targets and unbounded chains, and no block
 * translated yet. The caller keeps the PW_X86_ENGINE_CALL_STACK_GUARD bytes
 * below base inaccessible and sends host faults there to
 * pw_x86_engine_call_stack_fault. */
int pw_x86_engine_set_call_stack(PwX86Engine *, void *base, size_t bytes);
/* A host fault at address, in translated code: 1 when it is a call that ran
 * out of call stack, with *rsp set to resume it on an empty one (the calls
 * below lose their predicted returns, nothing else), else 0. */
int pw_x86_engine_call_stack_fault(const PwX86Engine *, uintptr_t address, uintptr_t *rsp);
/* Where to resume a host fault at rip: the refused-access path of the
 * marked access that faulted, or 0 when rip is not one (not ours). Safe in
 * a signal handler: it reads only the engine and its code. */
/* Resolve a sampled host PC using the arena's block map, including entry and
 * exit code. Requires fault markers and the owner thread's stable cache;
 * returns NULL for stubs, gaps, outside addresses and stale generations. */
const PwX86CacheEntry *pw_x86_engine_host_block(const PwX86Engine *, uintptr_t rip);
/* Sample only from the owner thread while its cache is stable. Reporting
 * must block the sampling signal; sampling allocates nothing and calls no
 * platform functions. Overflow is explicit and never replaces older rows. */
void pw_x86_engine_sample(const PwX86Engine *, uintptr_t rip, PwX86HotspotProfile *);
uintptr_t pw_x86_engine_fault_redirect(const PwX86Engine *, uintptr_t rip);
/* Leave the statistics counters out of blocks translated from now on (they
 * are on by default): a step then reports no retired instructions, and the
 * transition and register totals stay zero. The chain budget, the link
 * slots and every guest-visible effect are unchanged. */
int pw_x86_engine_set_counters(PwX86Engine *, unsigned enabled);
int pw_x86_engine_step(PwX86Engine *,PwX86State *,PwX86StepReport *);
/* Time generated-code invocation (including its FP wrapper), excluding
 * compilation, resets and dispatcher work. Totals survive cache resets.
 * stride=1 times every invocation; larger strides sample approximately 1/N
 * invocations. execution_ns sums samples only. NULL disables clock reads.
 * Set only from the engine's owner thread. */
int pw_x86_engine_set_execution_clock(PwX86Engine *, PwX86ExecutionClock, void *opaque, uint32_t stride);
int pw_x86_engine_reset(PwX86Engine *,uint32_t);
int pw_x86_engine_destroy(PwX86Engine *);
#endif
