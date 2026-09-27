/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * prospero-win WoW64 CPU backend, Unix side: owns one IA-32 DBT engine per
 * host thread and runs guest code from the canonical I386_CONTEXT until EIP
 * reaches a BOP address or the engine reports a fault.
 */
#if 0
#pragma makedep unix
#endif

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wowprospero.h"

#include "pw_x86_engine.h"
#include "pw_guest_fp.h"
#include "pw_x86_hostexec.h"
#include "code_pages.h"
#include "thread_budget.h"
#include "host_memory.h"

/* The guest range every translated access is checked against (load_state). */
enum { GUEST_LOW = 0x10000u, GUEST_HIGH = 0xfffff000u };
/* The guest GPRs held in host registers across linked blocks: seven fit, and
 * leaving out EDX measured best on 7-Zip (docs/DBT_BENCHMARK.md), unless
 * PW_WOW_RESIDENT names others. */
enum { GLOBAL_RESIDENT = 0xfb };
/* Linked blocks per dispatcher return (docs/DBT_BENCHMARK.md). */
enum { QUANTUM = 1024 };

struct pw_thread
{
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86State state;
    uint64_t generation;     /* code_generation this cache was built for */
    uint32_t cache_epoch;    /* engine generation, bumped on every reset */
    uint32_t trace;          /* PW_WOW_TRACE: quantum 1 and an EIP ring */
    uint32_t prefer_host;    /* PW_WOW_HOSTEXEC_ALL: reference execution */
    uint32_t ring[64], ring_pos;
    PwX86HostExec hostexec;  /* single-instruction fallback */
    /* Last committed, readable region NtQueryVirtualMemory reported, valid
     * for readable_generation: consecutive blocks rarely leave it. */
    uintptr_t readable_low, readable_high;
    uint64_t readable_generation;
    uint64_t readable_queries, readable_hits;
    PwX86CacheEntry *entries;  /* the budget's count (thread_budget.h) */
};

C_ASSERT( sizeof(((I386_CONTEXT *)0)->ExtendedRegisters) == PW_GUEST_FXSAVE_BYTES );

static __thread struct pw_thread *self;
static PwWowHostMemory host_memory;
/* Set once a thread has its DBT: the threads after it take the smaller
 * budget (thread_budget.h). */
static int first_thread_ready;
/* Bumped when a memory notification touches a page translated code was read
 * from: every thread then discards its translations on its next entry. */
static volatile uint64_t code_generation = 1;
/* Bumped by every memory notification: protection may have changed. */
static volatile uint64_t protect_generation = 1;
static PwX86CodePages code_pages;
static volatile int flush_lock;

/* Translation reads source bytes straight from the identity-mapped guest.
 * A span never extends into an unreadable page. Readability comes from
 * Wine's own view of the address space (NtQueryVirtualMemory is resolved in
 * process, without a kernel call), which is the same on every host; the
 * last readable region is cached per thread until the next memory
 * notification, because frees and protection changes bump
 * protect_generation. */
static int readable( uintptr_t address )
{
    struct pw_thread *thread = self;
    MEMORY_BASIC_INFORMATION info;
    const ULONG readable_mask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    uint64_t generation = __atomic_load_n( &protect_generation, __ATOMIC_ACQUIRE );

    if (thread && thread->readable_generation == generation &&
        address >= thread->readable_low && address < thread->readable_high)
    {
        thread->readable_hits++;
        return 1;
    }
    if (thread) thread->readable_queries++;
    if (NtQueryVirtualMemory( NtCurrentProcess(), (void *)address, MemoryBasicInformation,
                              &info, sizeof(info), NULL ))
        return 0;
    if (info.State != MEM_COMMIT || !(info.Protect & readable_mask) || (info.Protect & PAGE_GUARD))
        return 0;
    if (thread)
    {
        thread->readable_low = (uintptr_t)info.BaseAddress;
        thread->readable_high = (uintptr_t)info.BaseAddress + info.RegionSize;
        thread->readable_generation = generation;
    }
    return 1;
}

static int source_view( void *opaque, uint32_t pc, const uint8_t **source, size_t *bytes )
{
    const uintptr_t page = 0x1000;
    uintptr_t end = ((uintptr_t)pc | (page - 1)) + 1;

    if (pc < 0x10000 || !readable( pc )) return PW_ERR_NOT_FOUND;
    if (end - pc < PW_X86_ENGINE_MAX_SOURCE && end < 0x100000000ull && readable( end ))
        end += page;
    *source = (const uint8_t *)(uintptr_t)pc;
    *bytes = end - pc;
    if (*bytes > PW_X86_ENGINE_MAX_SOURCE) *bytes = PW_X86_ENGINE_MAX_SOURCE;
    /* Whatever a translation may read from; flush() keys on these marks. */
    pw_x86_code_pages_mark( &code_pages, pc, *bytes );
    return PW_OK;
}

/* Zeroed memory above the guest's 4 GiB, from Wine's own virtual memory
 * (host_memory.h); NULL when there is none. */
static void *allocate_above_guest( size_t bytes, ULONG protect )
{
    MEM_ADDRESS_REQUIREMENTS requirements = { (void *)0x100000000, NULL, 0 };
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    SIZE_T size = bytes;
    void *base = NULL;

    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    if (NtAllocateVirtualMemoryEx( NtCurrentProcess(), &base, &size, MEM_RESERVE | MEM_COMMIT,
                                   protect, &parameter, 1 ))
        return NULL;
    return base;
}

static void *allocate_code( size_t bytes )
{
    return allocate_above_guest( bytes, PAGE_EXECUTE_READWRITE );
}

static void release( void *base, size_t bytes )
{
    SIZE_T size = 0;

    NtFreeVirtualMemory( NtCurrentProcess(), &base, &size, MEM_RELEASE );
}

/* The thread's cache entries, engine and fallback within one budget; 0, or
 * -1 with nothing left allocated. */
static int setup_thread( void *context, const PwWowThreadBudget *budget )
{
    struct pw_thread *thread = context;
    const size_t entry_bytes = budget->entries * sizeof(*thread->entries);

    if (!(thread->entries = allocate_above_guest( entry_bytes, PAGE_READWRITE ))) return -1;
    if (pw_x86_engine_init( &thread->engine, &thread->vm, thread->entries, budget->entries,
                            budget->arena_bytes, (uint32_t)code_generation, source_view, NULL ) == PW_OK)
    {
        if (pw_x86_hostexec_init( &thread->hostexec, &thread->vm, budget->hostexec_bytes ) == PW_OK)
            return 0;
        pw_x86_engine_destroy( &thread->engine );
    }
    release( thread->entries, entry_bytes );
    thread->entries = NULL;
    return -1;
}

static struct pw_thread *get_thread(void)
{
    struct pw_thread *thread = self;
    PwWowThreadBudget budget;
    int first, attempt;

    if (thread) return thread;
    if (!(thread = calloc( 1, sizeof(*thread) ))) return NULL;
    first = !__atomic_load_n( &first_thread_ready, __ATOMIC_ACQUIRE );
    if (pw_wow_host_backend( &thread->vm, &host_memory ) != PW_OK ||
        (attempt = pw_wow_thread_fit( first, setup_thread, thread, &budget )) < 0)
    {
        fprintf( stderr, "wowprospero: no memory for a %s thread's translator\n",
                 first ? "first" : "further" );
        free( thread );
        return NULL;
    }
    if (attempt)
        fprintf( stderr, "wowprospero: %s thread's translator reduced to %u entries, %zu KiB of code\n",
                 first ? "first" : "further", budget.entries, budget.arena_bytes >> 10 );
    __atomic_store_n( &first_thread_ready, 1, __ATOMIC_RELEASE );
    {
        /* PW_WOW_MODES=<chaining><residency><lazy-flags>[<indirect>[<flat>[<reencode>]]],
         * e.g. "00000" for the plainest translation; used to bisect
         * optimisation defects. Digits left out stay on; indirect targets
         * take effect only with chaining; residency 2 is the per-block
         * allocator instead of the global one. */
        const char *modes = getenv( "PW_WOW_MODES" );
        size_t digits = modes ? strlen( modes ) : 0;

        if (digits < 3 || digits > 6) { modes = "111111"; digits = 6; }
        pw_x86_engine_set_chaining( &thread->engine, modes[0] == '1' );
        /* Residency '1': the guest GPRs in fixed host registers across
         * linked blocks (PW_WOW_RESIDENT=<hex mask> picks which); '2': the
         * older per-block allocator; '0': none. */
        pw_x86_engine_set_residency( &thread->engine, modes[1] != '0' );
        if (modes[1] == '1')
        {
            const char *mask = getenv( "PW_WOW_RESIDENT" );
            pw_x86_engine_set_global_resident( &thread->engine,
                                               mask ? (uint8_t)strtoul( mask, NULL, 16 ) : GLOBAL_RESIDENT );
        }
        pw_x86_engine_set_lazy_flags( &thread->engine, modes[2] == '1' );
        /* Without the table the dynamic exits simply return. */
        (void)pw_x86_engine_set_indirect( &thread->engine, digits < 4 || modes[3] != '0' );
        /* The guest range load_state gives the stack and the one region. */
        if (digits < 5 || modes[4] != '0')
            pw_x86_engine_set_flat_memory( &thread->engine, GUEST_LOW, GUEST_HIGH );
        /* The same-ISA re-encoder where it takes a block (it needs the flat
         * range and no counters); the emitter elsewhere. */
        pw_x86_engine_set_reencode( &thread->engine, digits < 6 || modes[5] != '0' );
        /* Nothing here reads the step statistics; PW_WOW_STATS keeps them. */
        pw_x86_engine_set_counters( &thread->engine, getenv( "PW_WOW_STATS" ) != NULL );
        /* A chain returns to this loop, which notices code flushes, after
         * QUANTUM linked blocks: at a few instructions a block that is tens
         * of microseconds. PW_WOW_QUANTUM overrides it. */
        {
            const char *quantum = getenv( "PW_WOW_QUANTUM" );
            unsigned long value = quantum ? strtoul( quantum, NULL, 10 ) : QUANTUM;
            pw_x86_engine_set_quantum( &thread->engine, value ? (uint32_t)value : QUANTUM );
        }
    }
    thread->prefer_host = getenv( "PW_WOW_HOSTEXEC_ALL" ) != NULL;
    if (getenv( "PW_WOW_TRACE" ))
    {
        thread->trace = 1;
        pw_x86_engine_set_quantum( &thread->engine, 1 );
    }
    thread->cache_epoch = (uint32_t)code_generation;
    pw_guest_fp_init( &thread->state.fp );
    thread->generation = code_generation;
    return self = thread;
}

static void load_state( PwX86State *state, const I386_CONTEXT *ctx, UINT teb32 )
{
    state->gpr[0] = ctx->Eax;
    state->gpr[1] = ctx->Ecx;
    state->gpr[2] = ctx->Edx;
    state->gpr[3] = ctx->Ebx;
    state->gpr[4] = ctx->Esp;
    state->gpr[5] = ctx->Ebp;
    state->gpr[6] = ctx->Esi;
    state->gpr[7] = ctx->Edi;
    state->eip = ctx->Eip;
    state->eflags = (ctx->EFlags & 0x00000cd5) | 0x2;
    state->deferred_flags.known_mask = 0;
    state->fs_base = teb32;
    state->fs_bytes = 0x1000;
    /* Wine owns the address space; every translated access is checked only
     * against the identity-mapped guest range and faults natively. */
    state->stack_low = GUEST_LOW;
    state->stack_high = GUEST_HIGH;
    state->memory_count = 1;
    state->memory[0].low = GUEST_LOW;
    state->memory[0].high = GUEST_HIGH;
    state->memory[0].permissions = PW_X86_READ | PW_X86_WRITE | PW_X86_EXEC;
}

/* The guest's x87 and SSE state is the thread's own hardware state whenever
 * the guest is not running, as with wow64cpu: cpu.c saves it into the
 * context's FXSAVE image just before this call and restores it from there
 * after. So what Wine's NtContinue, SetThreadContext and exception dispatch
 * write into the thread's FP state reaches the guest, and GetThreadContext
 * reads the guest's. In between, the image is the guest's. */
static void sync_fp_in( struct pw_thread *thread, const I386_CONTEXT *ctx )
{
    pw_guest_fp_from_fxsave( &thread->state.fp, ctx->ExtendedRegisters );
}

static void sync_fp_out( struct pw_thread *thread, I386_CONTEXT *ctx )
{
    pw_guest_fp_to_fxsave( &thread->state.fp, ctx->ExtendedRegisters );
}

static void store_state( const PwX86State *state, I386_CONTEXT *ctx )
{
    ctx->Eax = state->gpr[0];
    ctx->Ecx = state->gpr[1];
    ctx->Edx = state->gpr[2];
    ctx->Ebx = state->gpr[3];
    ctx->Esp = state->gpr[4];
    ctx->Ebp = state->gpr[5];
    ctx->Esi = state->gpr[6];
    ctx->Edi = state->gpr[7];
    ctx->Eip = state->eip;
    ctx->EFlags = (ctx->EFlags & ~0x00000cd5) | (state->eflags & 0x00000cd5);
}

static NTSTATUS process_init( void *args )
{
    long page = sysconf( _SC_PAGESIZE );

    host_memory.allocate = allocate_code;
    host_memory.release = release;
    host_memory.page = page > 0 ? (size_t)page : 0x1000;
    host_memory.alignment = 0x10000;  /* the allocation granularity */
    return STATUS_SUCCESS;
}

static NTSTATUS run( void *args )
{
    struct pw_wow_run_params *params = args;
    I386_CONTEXT *ctx = (I386_CONTEXT *)(ULONG_PTR)params->context;
    struct pw_thread *thread = get_thread();
    PwX86StepReport report;
    PwX86State *state;
    uint64_t generation;
    int status;

    if (!thread)
    {
        params->reason = PW_WOW_ERROR;
        params->status = PW_ERR_VM;
        return STATUS_SUCCESS;
    }
    state = &thread->state;
    generation = __atomic_load_n( &code_generation, __ATOMIC_ACQUIRE );
    if (thread->generation != generation)
    {
        thread->generation = generation;
        pw_x86_engine_reset( &thread->engine, ++thread->cache_epoch );
        pw_x86_hostexec_reset( &thread->hostexec );
    }
    load_state( state, ctx, params->teb32 );
    sync_fp_in( thread, ctx );
    for (;;)
    {
        if (state->eip == params->bop) { params->reason = PW_WOW_SYSCALL; break; }
        if (state->eip == params->unix_bop) { params->reason = PW_WOW_UNIXCALL; break; }
        if (thread->trace) thread->ring[thread->ring_pos++ & 63] = state->eip;
        if (thread->prefer_host)
        {
            /* Diagnostic: every instruction the host can execute bypasses the
             * translator, isolating translator semantics from everything else. */
            const uint8_t *source;
            size_t bytes;

            pw_x86_commit_canonical_flags( state );
            if (source_view( NULL, state->eip, &source, &bytes ) == PW_OK &&
                pw_x86_hostexec_step( &thread->hostexec, state, source, bytes ) == PW_OK)
                continue;
            pw_x86_engine_set_quantum( &thread->engine, 1 );
        }
        status = pw_x86_engine_step( &thread->engine, state, &report );
        if (status == PW_OK) continue;
        if (status == PW_ERR_UNSUPPORTED)
        {
            const uint8_t *source;
            size_t bytes;

            /* The block stopped before an instruction the translator does not
             * cover; run exactly that instruction on the host. */
            pw_x86_commit_canonical_flags( state );
            if (source_view( NULL, state->eip, &source, &bytes ) == PW_OK &&
                pw_x86_hostexec_step( &thread->hostexec, state, source, bytes ) == PW_OK)
                continue;
        }
        if (status == PW_ERR_LIMIT)
        {
            pw_x86_engine_reset( &thread->engine, ++thread->cache_epoch );
            continue;
        }
        params->status = status;
        params->fault_address = state->fault_address;
        params->fault_write = state->fault_write;
        if (status == PW_ERR_VM) params->reason = PW_WOW_FAULT;
        else if (status == PW_ERR_UNSUPPORTED) params->reason = PW_WOW_UNSUPPORTED;
        else if (status == PW_ERR_X87_TRAP) params->reason = PW_WOW_X87_TRAP;
        else params->reason = PW_WOW_ERROR;
        break;
    }
    pw_x86_commit_canonical_flags( state );
    store_state( state, ctx );
    sync_fp_out( thread, ctx );
    return STATUS_SUCCESS;
}

/* A memory notification. The loader protects and frees memory hundreds of
 * times while it maps and relocates DLLs; discarding every translation each
 * time made a game's startup re-translate the same loader code over and
 * over. Translations are discarded only when the range touches a page code
 * was translated from, or its extent is unknown (size 0: an unmapped view
 * or a whole-cache flush).
 *
 * Marks are cleared before the generation moves, under a lock, so a thread
 * that saw the new generation marks pages after the clear: a mark is lost
 * only for a thread that has yet to see the bump, which will discard its
 * translations anyway. */
static NTSTATUS flush( void *args )
{
    const struct pw_wow_flush_params *params = args;

    __atomic_add_fetch( &protect_generation, 1, __ATOMIC_SEQ_CST );
    while (__atomic_exchange_n( &flush_lock, 1, __ATOMIC_ACQUIRE )) __builtin_ia32_pause();
    if (!params || !params->size || pw_x86_code_pages_any( &code_pages, params->address, params->size ))
    {
        pw_x86_code_pages_clear( &code_pages );
        __atomic_add_fetch( &code_generation, 1, __ATOMIC_SEQ_CST );
    }
    __atomic_store_n( &flush_lock, 0, __ATOMIC_RELEASE );
    return STATUS_SUCCESS;
}

static NTSTATUS dump( void *args )
{
    struct pw_thread *thread = self;

    if (!thread) return STATUS_SUCCESS;
    fprintf( stderr, "wowprospero: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x eip=%08x fl=%08x\n",
             thread->state.gpr[0], thread->state.gpr[1], thread->state.gpr[2], thread->state.gpr[3],
             thread->state.gpr[4], thread->state.gpr[5], thread->state.gpr[6], thread->state.gpr[7],
             thread->state.eip, thread->state.eflags );
    for (uint32_t i = 0; thread->trace && i < 64; i++)
        fprintf( stderr, "wowprospero: ring[%u]=%08x\n", i,
                 thread->ring[(thread->ring_pos + i) & 63] );
    return STATUS_SUCCESS;
}

static NTSTATUS thread_term( void *args )
{
    struct pw_thread *thread = self;

    if (!thread) return STATUS_SUCCESS;
    self = NULL;
    pw_x86_hostexec_destroy( &thread->hostexec );
    pw_x86_engine_destroy( &thread->engine );
    release( thread->entries, 0 );
    free( thread );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_init,
    run,
    flush,
    thread_term,
    dump,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == pw_wow_funcs_count );
