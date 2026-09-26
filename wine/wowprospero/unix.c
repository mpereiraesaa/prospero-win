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
#include <sys/uio.h>
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
#include "pw_vm_posix.h"
#include "pw_guest_fp.h"

enum { CACHE_ENTRIES = 65536, ARENA_BYTES = 128u * 1024u * 1024u };

struct pw_thread
{
    PwVmBackend vm;
    PwX86Engine engine;
    PwX86State state;
    uint64_t generation;     /* code_generation this cache was built for */
    uint32_t cache_epoch;    /* engine generation, bumped on every reset */
    uint32_t trace;          /* PW_WOW_TRACE: quantum 1 and an EIP ring */
    uint32_t ring[64], ring_pos;
    PwX86CacheEntry entries[CACHE_ENTRIES];
};

static __thread struct pw_thread *self;
static volatile uint64_t code_generation = 1;

/* Translation reads source bytes straight from the identity-mapped guest.
 * A span never extends into an unreadable page. process_vm_readv is the
 * Linux host probe; a PS5 adapter needs its own page-readability query. */
static int readable( uintptr_t address )
{
    char byte;
    struct iovec local = { &byte, 1 }, remote = { (void *)address, 1 };

    return process_vm_readv( getpid(), &local, 1, &remote, 1, 0 ) == 1;
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
    return PW_OK;
}

static struct pw_thread *get_thread(void)
{
    struct pw_thread *thread = self;

    if (thread) return thread;
    if (!(thread = calloc( 1, sizeof(*thread) ))) return NULL;
    if (pw_vm_posix_backend( &thread->vm ) != PW_OK ||
        pw_x86_engine_init( &thread->engine, &thread->vm, thread->entries, CACHE_ENTRIES,
                            ARENA_BYTES, (uint32_t)code_generation, source_view, NULL ) != PW_OK)
    {
        free( thread );
        return NULL;
    }
    {
        /* PW_WOW_MODES=<chaining><residency><lazy-flags>, e.g. "000" for
         * the plainest translation; used to bisect optimisation defects. */
        const char *modes = getenv( "PW_WOW_MODES" );

        if (!modes || strlen( modes ) != 3) modes = "111";
        pw_x86_engine_set_chaining( &thread->engine, modes[0] == '1' );
        pw_x86_engine_set_residency( &thread->engine, modes[1] == '1' );
        pw_x86_engine_set_lazy_flags( &thread->engine, modes[2] == '1' );
    }
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
    state->stack_low = 0x10000;
    state->stack_high = 0xfffff000;
    state->memory_count = 1;
    state->memory[0].low = 0x10000;
    state->memory[0].high = 0xfffff000;
    state->memory[0].permissions = PW_X86_READ | PW_X86_WRITE | PW_X86_EXEC;
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
    return STATUS_SUCCESS;
}

static NTSTATUS run( void *args )
{
    struct pw_wow_run_params *params = args;
    I386_CONTEXT *ctx = (I386_CONTEXT *)(ULONG_PTR)params->context;
    struct pw_thread *thread = get_thread();
    PwX86StepReport report;
    PwX86State *state;
    int status;

    if (!thread)
    {
        params->reason = PW_WOW_ERROR;
        params->status = PW_ERR_VM;
        return STATUS_SUCCESS;
    }
    state = &thread->state;
    if (thread->generation != code_generation)
    {
        thread->generation = code_generation;
        pw_x86_engine_reset( &thread->engine, ++thread->cache_epoch );
    }
    load_state( state, ctx, params->teb32 );
    for (;;)
    {
        if (state->eip == params->bop) { params->reason = PW_WOW_SYSCALL; break; }
        if (state->eip == params->unix_bop) { params->reason = PW_WOW_UNIXCALL; break; }
        if (thread->trace) thread->ring[thread->ring_pos++ & 63] = state->eip;
        status = pw_x86_engine_step( &thread->engine, state, &report );
        if (status == PW_OK) continue;
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
    return STATUS_SUCCESS;
}

static NTSTATUS flush( void *args )
{
    __atomic_add_fetch( &code_generation, 1, __ATOMIC_SEQ_CST );
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
    pw_x86_engine_destroy( &thread->engine );
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
