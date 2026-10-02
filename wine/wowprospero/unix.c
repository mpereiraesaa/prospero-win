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
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#if defined(__linux__)
#include <ucontext.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <x86intrin.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wowprospero.h"

#include "pw_x86_engine.h"
#include "pw_x86_reencode.h"
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
/* Host return addresses of the guest calls in a chain (the engine's call
 * stack): 32768 nested calls, beyond which the stack starts again. */
enum { CALL_STACK = 0x40000 };

struct pw_thread
{
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86State state;
    uint64_t generation;     /* code_generation this cache was built for */
    uint32_t cache_epoch;    /* engine generation, bumped on every reset */
    uint64_t cache_publishes_at_reset, cache_report_id;
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
    void *call_stack;          /* guard, then CALL_STACK bytes, or NULL */
    /* PW_WOW_TIMING: TSC cycles spent inside run(), and outside it by the
     * reason the previous run returned for, since t_window. */
    uint64_t t_mark, t_window, t_inside, t_unix, t_sys, t_other, t_unix_long;
    uint64_t wall_window;
    double tsc_per_us;       /* measured at the last report */
    uint32_t n_unix, n_sys, n_other, n_unix_long, n_resets, n_flushes, last_reason;
    PwX86HotspotProfile *profile;
    uint64_t profile_last_dump;
    uint64_t execution_clock_cost, execution_clock_resolution;
};

C_ASSERT( sizeof(((I386_CONTEXT *)0)->ExtendedRegisters) == PW_GUEST_FXSAVE_BYTES );

static __thread struct pw_thread *self;
static uint64_t next_cache_report_id;

/* Fault markers (pw_x86_block.h): re-encoded blocks do not check their
 * accesses against the guest range; an access outside it faults on the
 * host, and the SIGSEGV handler below resumes the block at the path that
 * reports it to the guest, as the check did. Only such faults are taken:
 * one inside the guest range (a guard page, a write watch, memory Wine has
 * not mapped) stays Wine's, as before. The handler reads the translators'
 * code ranges from this table rather than thread-local storage. */
enum { MAX_ARENAS = 1024 };
static struct { uintptr_t low, high; PwX86Engine *engine; PwX86State *state; PwX86HotspotProfile *profile; } arenas[MAX_ARENAS];
#ifndef __PROSPERO__
static struct sigaction wine_segv;
#endif
static int fault_markers;
static void register_arena( struct pw_thread *thread, int add );
static void profile_add_thread( struct pw_thread *thread );
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
 * (host_memory.h), committed, or only reserved (type MEM_RESERVE); NULL
 * when there is none. */
static void *place_above_guest( size_t bytes, ULONG type, ULONG protect )
{
    MEM_ADDRESS_REQUIREMENTS requirements = { (void *)0x100000000, NULL, 0 };
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    SIZE_T size = bytes;
    void *base = NULL;

    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    if (NtAllocateVirtualMemoryEx( NtCurrentProcess(), &base, &size, type, protect, &parameter, 1 ))
        return NULL;
    return base;
}

static void *allocate_above_guest( size_t bytes, ULONG protect )
{
    return place_above_guest( bytes, MEM_RESERVE | MEM_COMMIT, protect );
}

static void *allocate_code( size_t bytes )
{
    return allocate_above_guest( bytes, PAGE_EXECUTE_READWRITE );
}

/* A code arena is reserved, and committed as translations fill it
 * (host_memory.h): on the console committed memory is direct memory. */
static void *reserve_code( size_t bytes )
{
    return place_above_guest( bytes, MEM_RESERVE, PAGE_NOACCESS );
}

static int commit_code( void *base, size_t bytes )
{
    SIZE_T size = bytes;

    return NtAllocateVirtualMemory( NtCurrentProcess(), &base, 0, &size, MEM_COMMIT,
                                    PAGE_EXECUTE_READWRITE ) ? -1 : 0;
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

static uint64_t execution_clock( void *opaque )
{
    struct timespec now;
    (void)opaque;
    if (clock_gettime( CLOCK_THREAD_CPUTIME_ID, &now )) return 0;
    return now.tv_sec * 1000000000ull + now.tv_nsec;
}

static void execution_report( struct pw_thread *thread )
{
    if (thread->engine.dispatch_profile)
        fprintf( stderr, "wowprospero dispatch: tid=%04x cumulative=1 table_matches=%llu table_empty=%llu table_collisions=%llu\n",
                 (unsigned)(uintptr_t)NtCurrentTeb()->ClientId.UniqueThread,
                 (unsigned long long)thread->engine.dispatch_chain_matches,
                 (unsigned long long)thread->engine.dispatch_chain_empty,
                 (unsigned long long)thread->engine.dispatch_chain_collisions );
    if (!thread->engine.execution_clock) return;
    fprintf( stderr, "wowprospero execution: tid=%04x cumulative=1 sample_cpu_ns=%llu calls=%llu samples=%llu stride=%u clock_batch_read_ns=%llu clock_resolution_ns=%llu clock_errors=%llu\n",
             (unsigned)(uintptr_t)NtCurrentTeb()->ClientId.UniqueThread,
             (unsigned long long)thread->engine.execution_ns,
             (unsigned long long)thread->engine.execution_calls,
             (unsigned long long)thread->engine.execution_samples,
             thread->engine.execution_stride,
             (unsigned long long)thread->execution_clock_cost,
             (unsigned long long)thread->execution_clock_resolution,
             (unsigned long long)thread->engine.execution_clock_errors );
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
        /* Accesses outside the flat range fault instead of being checked. */
        if (fault_markers && (digits < 5 || modes[4] != '0'))
        {
            register_arena( thread, 1 );
            pw_x86_engine_set_fault_markers( &thread->engine, 1 );
        }
        /* Nothing here reads the step statistics; PW_WOW_STATS keeps them. */
        pw_x86_engine_set_counters( &thread->engine, getenv( "PW_WOW_STATS" ) != NULL );
        /* Blocks from the older emitter return to this loop after QUANTUM
         * linked blocks. PW_WOW_QUANTUM overrides it, and also makes
         * re-encoded blocks spend it (below). */
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
    /* Nothing here needs the dispatcher between linked blocks: code flushes
     * are noticed when run() starts, and Wine suspends a thread with a
     * signal. So re-encoded chains spend no budget, unless a mode above
     * steps block by block or PW_WOW_QUANTUM asks for one. */
    pw_x86_engine_set_unbounded_chains( &thread->engine, !thread->trace && !thread->prefer_host &&
                                        !getenv( "PW_WOW_QUANTUM" ) );
    /* Blocks go on past conditional branches, whose side exits link
     * themselves in the code memory, which stays writable here
     * (PW_WOW_SUPERBLOCKS=0 ends blocks at every branch). */
    /* The guest's x87, MMX and SSE state runs in the host FPU in re-encoded
     * code, which copies FP and SIMD instructions (PW_WOW_NATIVE_FP=0 leaves
     * them to the emitter's software FPU). */
    pw_x86_engine_set_native_fp( &thread->engine, thread->engine.reencode_enabled &&
                                 (!getenv( "PW_WOW_NATIVE_FP" ) || strcmp( getenv( "PW_WOW_NATIVE_FP" ), "0" )) );
    pw_x86_engine_set_superblocks( &thread->engine, thread->engine.unbounded_chains &&
                                   (!getenv( "PW_WOW_SUPERBLOCKS" ) || strcmp( getenv( "PW_WOW_SUPERBLOCKS" ), "0" )) );
    /* Calls and returns on a call stack, so the host predicts the returns
     * (PW_WOW_CALL_STACK=0 keeps the lookup); its guard sends a call that
     * runs out of it to redirect_fault. */
    if (thread->engine.unbounded_chains && thread->engine.reencode_enabled && fault_markers &&
        (!getenv( "PW_WOW_CALL_STACK" ) || strcmp( getenv( "PW_WOW_CALL_STACK" ), "0" )) &&
        (thread->call_stack = allocate_above_guest( PW_X86_ENGINE_CALL_STACK_GUARD + CALL_STACK,
                                                    PAGE_READWRITE )))
    {
        void *guard = thread->call_stack;
        SIZE_T size = PW_X86_ENGINE_CALL_STACK_GUARD;
        ULONG old;

        if (NtProtectVirtualMemory( NtCurrentProcess(), &guard, &size, PAGE_NOACCESS, &old ) ||
            pw_x86_engine_set_call_stack( &thread->engine, (char *)thread->call_stack +
                                          PW_X86_ENGINE_CALL_STACK_GUARD, CALL_STACK ) != PW_OK)
        {
            release( thread->call_stack, 0 );
            thread->call_stack = NULL;
        }
    }
    thread->cache_epoch = (uint32_t)code_generation;
    thread->cache_report_id = __atomic_add_fetch( &next_cache_report_id, 1, __ATOMIC_RELAXED );
    pw_guest_fp_init( &thread->state.fp );
    thread->generation = code_generation;
    {
        int dispatch = getenv( "PW_WOW_DISPATCH_PROFILE" ) != NULL;
        int enabled = getenv( "PW_WOW_EXEC_TIMING" ) != NULL;
#ifdef PW_WOW_TIMING_TRIGGER
        struct stat dispatch_st;
        if (!stat( "/data/prospero-win/pw_wow_dispatch_profile", &dispatch_st )) dispatch = 1;
#endif
        pw_x86_engine_set_dispatch_profile( &thread->engine, dispatch );
#ifdef __PROSPERO__
        struct stat st;
        if (!stat( "/data/prospero-win/pw_wow_exec_timing", &st )) enabled = 1;
#endif
        if (enabled)
        {
            const char *option = getenv( "PW_WOW_EXEC_STRIDE" );
            unsigned long stride = option ? strtoul( option, NULL, 10 ) : 64;
            if (!stride || stride > UINT32_MAX) stride = 64;
            struct timespec resolution;
            if (!clock_getres( CLOCK_THREAD_CPUTIME_ID, &resolution ))
                thread->execution_clock_resolution = resolution.tv_sec * 1000000000ull + resolution.tv_nsec;
            if (pw_x86_execution_clock_batch( execution_clock, NULL, &thread->execution_clock_cost ) == PW_OK)
                pw_x86_engine_set_execution_clock( &thread->engine, execution_clock, NULL, (uint32_t)stride );
            else fprintf( stderr, "wowprospero execution: unavailable thread CPU clock; timing disabled\n" );
        }
    }
    profile_add_thread( thread );
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
    state->selector[0] = LOWORD(ctx->SegEs);
    state->selector[1] = LOWORD(ctx->SegCs);
    state->selector[2] = LOWORD(ctx->SegSs);
    state->selector[3] = LOWORD(ctx->SegDs);
    state->selector[4] = LOWORD(ctx->SegFs);
    state->selector[5] = LOWORD(ctx->SegGs);
}

/* The guest's x87 and SSE state is the thread's own hardware state whenever
 * the guest is not running, as with wow64cpu: cpu.c saves it into the
 * context's FXSAVE image just before this call and restores it from there
 * after. So what Wine's NtContinue, SetThreadContext and exception dispatch
 * write into the thread's FP state reaches the guest, and GetThreadContext
 * reads the guest's. In between, the image is the guest's. */
static void sync_fp_in( struct pw_thread *thread, const I386_CONTEXT *ctx )
{
    pw_x86_engine_fp_load( &thread->engine, &thread->state, (const uint8_t *)ctx->ExtendedRegisters );
}

static void sync_fp_out( struct pw_thread *thread, I386_CONTEXT *ctx )
{
    pw_x86_engine_fp_store( &thread->engine, &thread->state, (uint8_t *)ctx->ExtendedRegisters );
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

/* The saved RIP in a signal context, or NULL where it is not known. */
/* The saved RSP, in the same layout. */
static uintptr_t *context_rsp( void *context )
{
#if defined(__PROSPERO__)
    return (uintptr_t *)((char *)context + 248);  /* mcontext at 64, mc_rsp at 184 */
#elif defined(__linux__)
    return (uintptr_t *)&((ucontext_t *)context)->uc_mcontext.gregs[REG_RSP];
#elif defined(__FreeBSD__)
    return (uintptr_t *)&((ucontext_t *)context)->uc_mcontext.mc_rsp;
#else
    (void)context;
    return NULL;
#endif
}

static uintptr_t *context_rip( void *context )
{
#if defined(__PROSPERO__)
    return (uintptr_t *)((char *)context + 224);  /* the live slot (WINE_INTEGRATION.md) */
#elif defined(__linux__)
    return (uintptr_t *)&((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__FreeBSD__)
    return (uintptr_t *)&((ucontext_t *)context)->uc_mcontext.mc_rip;
#else
    (void)context;
    return NULL;
#endif
}

/* Resumes a fault at a marked access outside the guest range at the
 * access's refused-access path; nonzero when it did. */
static int redirect_fault( siginfo_t *info, void *context )
{
    const uintptr_t address = (uintptr_t)info->si_addr;
    uintptr_t *rip = context_rip( context ), target = 0;
    PwX86State *state = NULL;
    uint32_t eip;

    for (unsigned int i = 0; i < MAX_ARENAS && !target; i++)
    {
        uintptr_t high = __atomic_load_n( &arenas[i].high, __ATOMIC_ACQUIRE );
        uintptr_t low = __atomic_load_n( &arenas[i].low, __ATOMIC_ACQUIRE );
        if (low && high && *rip >= low && *rip < high)
        {
            uintptr_t rsp;
            /* A call that ran out of call stack: start it again. */
            if (context_rsp( context ) &&
                pw_x86_engine_call_stack_fault( arenas[i].engine, address, &rsp ))
            {
                *context_rsp( context ) = rsp;
                return 1;
            }
            target = pw_x86_engine_fault_redirect( arenas[i].engine, *rip );
            state = arenas[i].state;
        }
    }
    if (!target) return 0;
    if (address >= GUEST_LOW && address < GUEST_HIGH)
    {
        /* Wine's to handle (a guard page, a write watch); the block stored
         * no EIP before the access, so give Wine the exact one. */
        if (state && pw_x86_cold_path_eip( target, &eip )) state->eip = eip;
        return 0;
    }
    *rip = target;
    return 1;
}

#ifdef __PROSPERO__
/* An access violation Wine could not resolve, at a marked access in our code
 * (a real guest fault, not a guard page or write watch): resume it at the
 * access's refused-access path, which leaves translated code with the exact
 * EIP and address for BTCpuSimulate to raise as the guest's own exception.
 * Wine cannot dispatch it itself: translated code runs on our stack, outside
 * the thread's kernel stack (patch 0710). */
static int redirect_unresolved( siginfo_t *info, void *context )
{
    uintptr_t *rip = context_rip( context ), target = 0;

    (void)info;
    for (unsigned int i = 0; i < MAX_ARENAS && !target; i++)
    {
        uintptr_t high = __atomic_load_n( &arenas[i].high, __ATOMIC_ACQUIRE );
        uintptr_t low = __atomic_load_n( &arenas[i].low, __ATOMIC_ACQUIRE );
        if (low && high && *rip >= low && *rip < high)
            target = pw_x86_engine_fault_redirect( arenas[i].engine, *rip );
    }
    if (!target) return 0;
    *rip = target;
    return 1;
}

/* On the PS5, Wine's own handler calls redirect_fault first (patch 0610), and
 * redirect_unresolved for what it could not resolve (patch 0710). */
extern void __wine_ps5_set_segv_hook( int (*hook)( siginfo_t *info, void *context ) );
extern void __wine_ps5_set_segv_unresolved_hook( int (*hook)( siginfo_t *info, void *context ) );
#else
static void segv_handler( int signal, siginfo_t *info, void *context )
{
    if (redirect_fault( info, context )) return;
    if (wine_segv.sa_flags & SA_SIGINFO) wine_segv.sa_sigaction( signal, info, context );
    else if (wine_segv.sa_handler != SIG_DFL && wine_segv.sa_handler != SIG_IGN) wine_segv.sa_handler( signal );
    else sigaction( SIGSEGV, &wine_segv, NULL );  /* the default action on the retry */
}
#endif

/* Profiles belong to the sampled thread. Resolve the PC immediately, before
 * cache invalidation can reuse its arena, and retain only addresses/counts.
 * No cache of another thread or proprietary guest bytes is read by reporting. */
static const char *profile_path;
static uint64_t profile_ticks, profile_unattributed;
#include <sys/time.h>
#ifndef __PROSPERO__
#include <dlfcn.h>
#endif
/* Native PCs are process-wide and cumulative. Keys never move or disappear;
 * atomic publication/counts allow signals on different threads to sample
 * without a lock, TLS, or access to another thread's cache. */
enum { PROFILE_NATIVE_SLOTS = 4096 };
static struct profile_native { uintptr_t pc; uint64_t samples; } profile_native[PROFILE_NATIVE_SLOTS];
static uint64_t profile_native_overflow, profile_native_last_dump;
static void profile_native_sample(uintptr_t pc)
{
    unsigned bucket = (unsigned)((pc >> 4) * 2654435761u) & (PROFILE_NATIVE_SLOTS - 1);
    if(!pc) return;
    for(unsigned i = 0; i < PROFILE_NATIVE_SLOTS; i++) {
        uintptr_t key = __atomic_load_n(&profile_native[bucket].pc, __ATOMIC_ACQUIRE);
        if(!key) {
            uintptr_t empty = 0;
            if(__atomic_compare_exchange_n(&profile_native[bucket].pc, &empty, pc, 0,
                                           __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) key = pc;
            else key = empty;
        }
        if(key == pc) {
            __atomic_fetch_add(&profile_native[bucket].samples, 1, __ATOMIC_RELAXED);
            return;
        }
        bucket = (bucket + 1) & (PROFILE_NATIVE_SLOTS - 1);
    }
    __atomic_fetch_add(&profile_native_overflow, 1, __ATOMIC_RELAXED);
}
static void profile_handler(int signal, siginfo_t *info, void *context)
{
    const uintptr_t rip = *context_rip(context);
    (void)signal; (void)info;
    __atomic_fetch_add(&profile_ticks, 1, __ATOMIC_RELAXED);
    /* Wine may change FS before entering translated code: compiler TLS access
     * (including __tls_get_addr) is unsafe here. Locate the interrupted arena
     * instead. Only its owner can execute it, so no other cache is inspected. */
    for(unsigned i = 0; i < MAX_ARENAS; i++) {
        uintptr_t low = __atomic_load_n(&arenas[i].low, __ATOMIC_ACQUIRE);
        if(low && rip >= low && rip < __atomic_load_n(&arenas[i].high, __ATOMIC_ACQUIRE)) {
            PwX86HotspotProfile *profile = __atomic_load_n(&arenas[i].profile, __ATOMIC_ACQUIRE);
            if(profile) {
                pw_x86_engine_sample(arenas[i].engine, rip, profile);
                return;
            }
            break;
        }
    }
    __atomic_fetch_add(&profile_unattributed, 1, __ATOMIC_RELAXED);
    profile_native_sample(rip);
}

static void profile_start(void)
{
    profile_path = getenv("PW_WOW_PROFILE");
#ifdef __PROSPERO__
    if(!profile_path) {
        struct stat st;
        if(!stat("/data/prospero-win/pw_wow_profile", &st)) profile_path = "1";
    }
#endif
    if(!profile_path || !*profile_path) { profile_path = NULL; return; }
    struct sigaction action;
    struct itimerval timer = { { 0, 1000 }, { 0, 1000 } };
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = profile_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGPROF, &action, NULL) || setitimer(ITIMER_PROF, &timer, NULL)) {
        fprintf(stderr, "wowprospero profile: sampling timer unavailable\n");
        profile_path = NULL;
    }
}

static void profile_add_thread(struct pw_thread *thread)
{
    if(profile_path && !thread->engine.fault_markers) {
        fprintf(stderr, "wowprospero profile: requires fault-marker block map\n");
        return;
    }
    if(profile_path) {
        thread->profile = calloc(1, sizeof(*thread->profile));
        for(unsigned i = 0; i < MAX_ARENAS; i++)
            if(__atomic_load_n(&arenas[i].high, __ATOMIC_ACQUIRE) && arenas[i].engine == &thread->engine) {
                __atomic_store_n(&arenas[i].profile, thread->profile, __ATOMIC_RELEASE);
                break;
            }
    }
}

static int compare_hotspots(const void *a, const void *b)
{
    const PwX86Hotspot *x = a, *y = b;
    return x->samples < y->samples ? 1 : x->samples > y->samples ? -1 : 0;
}

static int compare_native_samples(const void *a, const void *b)
{
    const struct profile_native *x = a, *y = b;
    return x->samples < y->samples ? 1 : x->samples > y->samples ? -1 : 0;
}

static void profile_native_dump(uint64_t now, FILE *out)
{
    uint64_t last = __atomic_load_n(&profile_native_last_dump, __ATOMIC_RELAXED);
    struct profile_native *rows;
    if(now - last < 5000 || !__atomic_compare_exchange_n(&profile_native_last_dump, &last, now, 0,
                                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    rows = malloc(sizeof(profile_native));
    if(!rows) return;
    for(unsigned i = 0; i < PROFILE_NATIVE_SLOTS; i++) {
        rows[i].pc = __atomic_load_n(&profile_native[i].pc, __ATOMIC_ACQUIRE);
        rows[i].samples = __atomic_load_n(&profile_native[i].samples, __ATOMIC_RELAXED);
    }
    qsort(rows, PROFILE_NATIVE_SLOTS, sizeof(*rows), compare_native_samples);
    fprintf(out, "wowprospero native_summary: cumulative=1 overflow=%llu\n",
            (unsigned long long)__atomic_load_n(&profile_native_overflow, __ATOMIC_RELAXED));
    for(unsigned i = 0; i < 12 && rows[i].samples; i++) {
        const char *module = "?", *symbol = "?";
        uintptr_t offset = rows[i].pc;
#ifndef __PROSPERO__
        Dl_info info;
        if(dladdr((void *)rows[i].pc, &info)) {
            if(info.dli_fname) module = info.dli_fname;
            if(info.dli_sname) symbol = info.dli_sname;
            offset -= (uintptr_t)(info.dli_saddr ? info.dli_saddr : info.dli_fbase);
        }
#endif
        fprintf(out, "wowprospero native: pc=%#lx samples=%llu module=%s symbol=%s offset=%#lx\n",
                (unsigned long)rows[i].pc, (unsigned long long)rows[i].samples,
                module, symbol, (unsigned long)offset);
    }
    free(rows);
}

static void profile_maybe_dump(void)
{
    struct pw_thread *thread = self;
    struct timespec now;
    sigset_t mask, previous;
    PwX86HotspotProfile *snapshot;
    uint64_t ms, interval, entry_samples = 0, body_samples = 0, exit_samples = 0, emitted_samples = 0;
    unsigned tid = HandleToULong(NtCurrentTeb()->ClientId.UniqueThread);
    FILE *out = stderr;
    char path[512];

    if(!thread || !thread->profile) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = now.tv_sec * 1000ull + now.tv_nsec / 1000000;
    if(!thread->profile_last_dump) { thread->profile_last_dump = ms; return; }
    interval = ms - thread->profile_last_dump;
    if(interval < 5000) return;
    thread->profile_last_dump = ms;
    snapshot = malloc(sizeof(*snapshot));
    if(!snapshot) return;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPROF);
    if(sigprocmask(SIG_BLOCK, &mask, &previous)) { free(snapshot); return; }
    memcpy(snapshot, thread->profile, sizeof(*snapshot));
    memset(thread->profile, 0, sizeof(*thread->profile));
    sigprocmask(SIG_SETMASK, &previous, NULL);
    for(unsigned i = 0; i < PW_X86_HOTSPOT_SLOTS; i++) {
        entry_samples += snapshot->slots[i].entry;
        body_samples += snapshot->slots[i].body;
        exit_samples += snapshot->slots[i].exit;
        emitted_samples += snapshot->slots[i].emitted;
    }
    qsort(snapshot->slots, PW_X86_HOTSPOT_SLOTS, sizeof(snapshot->slots[0]), compare_hotspots);
#ifndef __PROSPERO__
    if(strcmp(profile_path, "1")) {
        snprintf(path, sizeof(path), "%s.%04x", profile_path, tid);
        out = fopen(path, "w");
        if(!out) out = stderr;
    }
#else
    (void)path;
#endif
    fprintf(out, "wowprospero profile: tid=%04x interval_ms=%llu samples=%llu outside=%llu stubs=%llu overflow=%llu\n",
            tid, (unsigned long long)interval, (unsigned long long)snapshot->samples, (unsigned long long)snapshot->outside,
            (unsigned long long)snapshot->stubs, (unsigned long long)snapshot->overflow);
    fprintf(out, "wowprospero profile_process: ticks=%llu unattributed=%llu\n",
            (unsigned long long)__atomic_load_n(&profile_ticks, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&profile_unattributed, __ATOMIC_RELAXED));
    fprintf(out, "wowprospero profile_parts: tid=%04x entry=%llu body=%llu exit=%llu emitted=%llu\n",
            tid, (unsigned long long)entry_samples, (unsigned long long)body_samples,
            (unsigned long long)exit_samples, (unsigned long long)emitted_samples);
    for(unsigned i = 0; i < 20 && snapshot->slots[i].samples; i++) {
        const PwX86Hotspot *row = &snapshot->slots[i];
        fprintf(out, "wowprospero hotspot: tid=%04x pc=%08x samples=%llu entry=%llu body=%llu exit=%llu emitted=%llu\n",
                tid, row->guest_pc, (unsigned long long)row->samples, (unsigned long long)row->entry,
                (unsigned long long)row->body, (unsigned long long)row->exit, (unsigned long long)row->emitted);
    }
    profile_native_dump(ms, out);
    if(out != stderr) fclose(out);
    free(snapshot);
}

static void register_arena( struct pw_thread *thread, int add )
{
    const uintptr_t low = (uintptr_t)thread->engine.code.exec_base;

    for (unsigned int i = 0; i < MAX_ARENAS; i++)
    {
        if (add)
        {
            /* Claim a free slot, then publish it: the handler skips a slot
             * whose high is still 0. */
            uintptr_t none = 0;
            if (__atomic_compare_exchange_n( &arenas[i].low, &none, low, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
            {
                __atomic_store_n(&arenas[i].profile, NULL, __ATOMIC_RELEASE);
                arenas[i].engine = &thread->engine;
                arenas[i].state = &thread->state;
                __atomic_store_n( &arenas[i].high, low + thread->engine.code.bytes, __ATOMIC_RELEASE );
                return;
            }
        }
        else if (__atomic_load_n( &arenas[i].low, __ATOMIC_ACQUIRE ) == low)
        {
            __atomic_store_n( &arenas[i].high, 0, __ATOMIC_RELEASE );
            __atomic_store_n( &arenas[i].profile, NULL, __ATOMIC_RELEASE );
            __atomic_store_n( &arenas[i].low, 0, __ATOMIC_RELEASE );
            return;
        }
    }
}

static NTSTATUS process_init( void *args )
{
    long page = sysconf( _SC_PAGESIZE );

    host_memory.allocate = allocate_code;
    host_memory.release = release;
    host_memory.reserve = reserve_code;
    host_memory.commit = commit_code;
    host_memory.lazy_bytes = (size_t)8 << 20;  /* the code arenas */
    host_memory.page = page > 0 ? (size_t)page : 0x1000;
    host_memory.alignment = 0x10000;  /* the allocation granularity */
    {
        /* PW_WOW_FAULT_MARKERS=0 keeps the checks. */
        const char *markers = getenv( "PW_WOW_FAULT_MARKERS" );

        if (!markers || strcmp( markers, "0" ))
        {
#ifdef __PROSPERO__
            __wine_ps5_set_segv_hook( redirect_fault );
            __wine_ps5_set_segv_unresolved_hook( redirect_unresolved );
            fault_markers = 1;
#else
            struct sigaction action;

            memset( &action, 0, sizeof(action) );
            if (context_rip( &action ) && !sigaction( SIGSEGV, NULL, &wine_segv ))
            {
                action.sa_sigaction = segv_handler;
                action.sa_mask = wine_segv.sa_mask;
                action.sa_flags = wine_segv.sa_flags | SA_SIGINFO | SA_ONSTACK;
                fault_markers = !sigaction( SIGSEGV, &action, NULL );
            }
#endif
        }
    }
    profile_start();
    return STATUS_SUCCESS;
}

/* PW_WOW_TIMING=1 (in a title, which passes Wine no such variable: the
 * trigger file below) times every thread with the TSC, without signals.
 * Every few seconds each thread that crossed to the host often logs how its
 * wall time split between run() (translated code and the translator) and the
 * time outside it after a Unix call (OpenGL, Vulkan, audio...) or a system
 * call, with the rate and mean cost of each, and how often the translations
 * were discarded. */
#ifdef __PROSPERO__
#define PW_WOW_TIMING_TRIGGER "/data/prospero-win/pw_wow_timing"
#endif
static int timing_enabled = -1;

static uint64_t timing_now_ns(void)
{
    struct timespec now;

    clock_gettime( CLOCK_MONOTONIC, &now );
    return now.tv_sec * 1000000000ull + now.tv_nsec;
}

static void timing_init(void)
{
    timing_enabled = getenv( "PW_WOW_TIMING" ) != NULL || getenv( "PW_WOW_EXEC_TIMING" ) != NULL ||
                     getenv( "PW_WOW_DISPATCH_PROFILE" ) != NULL;
#ifdef PW_WOW_TIMING_TRIGGER
    {
        struct stat st;  /* access() is refused to a title */

        if (!stat( PW_WOW_TIMING_TRIGGER, &st )) timing_enabled = 1;
        if (!stat( "/data/prospero-win/pw_wow_exec_timing", &st )) timing_enabled = 1;
        if (!stat( "/data/prospero-win/pw_wow_dispatch_profile", &st )) timing_enabled = 1;
    }
#endif
}

/* Owner-thread counters only: no table walk, signal-handler work or extra
 * per-lookup accounting. Publishes survive reset; occupancy does not. */
static void cache_report( struct pw_thread *thread, uint64_t wall, unsigned final )
{
    const PwX86Cache *cache = &thread->engine.cache;

    fprintf( stderr, "wowprospero cache: tid=%04x instance=%llu cumulative=1 time_ns=%llu final=%u "
             "generation=%u capacity=%u occupied=%llu arena_used=%zu arena_bytes=%zu "
             "hits=%llu misses=%llu probes=%llu max_probe=%u publishes=%llu resets=%llu\n",
             (unsigned)(uintptr_t)NtCurrentTeb()->ClientId.UniqueThread,
             (unsigned long long)thread->cache_report_id, (unsigned long long)wall, final,
             cache->generation, cache->capacity,
             (unsigned long long)(cache->publishes - thread->cache_publishes_at_reset),
             cache->cursor, cache->arena_bytes, (unsigned long long)cache->hits,
             (unsigned long long)cache->misses, (unsigned long long)cache->lookup_probes,
             cache->max_probe, (unsigned long long)cache->publishes,
             (unsigned long long)cache->resets );
}

static void timing_report( struct pw_thread *thread, uint64_t tsc )
{
    uint64_t wall = timing_now_ns();
    double cycles = (double)(tsc - thread->t_window);
    double seconds = (wall - thread->wall_window) / 1e9;
    double per_us = cycles / seconds / 1e6;

    execution_report( thread );

    cache_report( thread, wall, 0 );
    if (thread->n_unix + thread->n_sys > 1000)
        fprintf( stderr, "wowprospero timing: tid=%04x run=%.1f%% unix=%.1f%% (%.0f/s %.2fus) "
                 "sys=%.1f%% (%.0f/s %.2fus) other=%.1f%% (%u) unix_over_1ms=%.1f%% (%u) resets=%u flushes=%u\n",
                 (unsigned)(uintptr_t)NtCurrentTeb()->ClientId.UniqueThread,
                 100.0 * thread->t_inside / cycles,
                 100.0 * thread->t_unix / cycles, thread->n_unix / seconds,
                 thread->n_unix ? thread->t_unix / per_us / thread->n_unix : 0.0,
                 100.0 * thread->t_sys / cycles, thread->n_sys / seconds,
                 thread->n_sys ? thread->t_sys / per_us / thread->n_sys : 0.0,
                 100.0 * thread->t_other / cycles, thread->n_other,
                 100.0 * thread->t_unix_long / cycles, thread->n_unix_long, thread->n_resets, thread->n_flushes );
    thread->t_window = tsc;
    thread->wall_window = wall;
    thread->t_inside = thread->t_unix = thread->t_sys = thread->t_other = thread->t_unix_long = 0;
    thread->n_unix_long = 0;
    thread->tsc_per_us = per_us;
    thread->n_unix = thread->n_sys = thread->n_other = thread->n_resets = thread->n_flushes = 0;
}

/* At run()'s start: the time since the previous run returned. */
static void timing_enter( struct pw_thread *thread )
{
    uint64_t tsc = __rdtsc(), outside = tsc - thread->t_mark;

    if (!thread->t_window)
    {
        thread->t_window = tsc;
        thread->wall_window = timing_now_ns();
    }
    else if (thread->last_reason == PW_WOW_UNIXCALL)
    {
        /* A call that blocks (a wait for the display, say) rather than works. */
        thread->t_unix += outside;
        thread->n_unix++;
        if (thread->tsc_per_us && outside > 1000 * thread->tsc_per_us)
        {
            thread->t_unix_long += outside;
            thread->n_unix_long++;
        }
    }
    else if (thread->last_reason == PW_WOW_SYSCALL) { thread->t_sys += outside; thread->n_sys++; }
    else { thread->t_other += outside; thread->n_other++; }
    thread->t_mark = tsc;
}

/* At run()'s return; a report about every 2^34 cycles (5 to 10 s). */
static void timing_leave( struct pw_thread *thread, uint32_t reason )
{
    uint64_t tsc = __rdtsc();

    thread->t_inside += tsc - thread->t_mark;
    thread->t_mark = tsc;
    thread->last_reason = reason;
    if (tsc - thread->t_window > (1ull << 34)) timing_report( thread, tsc );
}

static void reset_thread_engine( struct pw_thread *thread )
{
    uint64_t resets = thread->engine.cache.resets;

    pw_x86_engine_reset( &thread->engine, ++thread->cache_epoch );
    if (thread->engine.cache.resets != resets)
        thread->cache_publishes_at_reset = thread->engine.cache.publishes;
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
    if (timing_enabled < 0) timing_init();
    if (timing_enabled) timing_enter( thread );
    generation = __atomic_load_n( &code_generation, __ATOMIC_ACQUIRE );
    if (thread->generation != generation)
    {
        thread->n_flushes++;
        thread->generation = generation;
        reset_thread_engine( thread );
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
            pw_x86_engine_fp_sync( &thread->engine, state );
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
            pw_x86_engine_fp_sync( &thread->engine, state );
            if (source_view( NULL, state->eip, &source, &bytes ) == PW_OK &&
                pw_x86_hostexec_step( &thread->hostexec, state, source, bytes ) == PW_OK)
                continue;
        }
        if (status == PW_ERR_LIMIT)
        {
            thread->n_resets++;
            pw_x86_engine_fp_sync( &thread->engine, state );
            reset_thread_engine( thread );
            continue;
        }
        pw_wow_report_error(params, status, state->eip, state->fault_address, state->fault_write);
        break;
    }
    pw_x86_commit_canonical_flags( state );
    store_state( state, ctx );
    sync_fp_out( thread, ctx );
    if (timing_enabled) timing_leave( thread, params->reason );
    profile_maybe_dump();
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
    execution_report( thread );
    if (timing_enabled > 0) cache_report( thread, timing_now_ns(), 1 );
    self = NULL;
    if (thread->engine.fault_markers) register_arena( thread, 0 );
    free(thread->profile);
    pw_x86_hostexec_destroy( &thread->hostexec );
    pw_x86_engine_destroy( &thread->engine );
    release( thread->entries, 0 );
    if (thread->call_stack) release( thread->call_stack, 0 );
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
