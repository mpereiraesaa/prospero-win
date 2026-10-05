/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * prospero-win WoW64 CPU backend, PE side.
 *
 * Wine's wow64.dll selects the i386 CPU through
 * HKLM\Software\Microsoft\Wow64\x86 and calls the BTCpu* contract that
 * wow64cpu.dll (hardware compat mode), xtajit.dll and third-party emulators
 * implement. This module keeps the canonical I386_CONTEXT where Wine expects
 * it (TlsSlots[WOW64_TLS_CPURESERVED] + 4) and runs guest code through the
 * prospero-win IA-32 DBT in its Unix library. The guest leaves the DBT only at
 * the two BOP addresses, which are serviced exactly as wow64cpu's
 * syscall_32to64 and unix_call_32to64 do.
 *
 * Derived from Wine dlls/wow64cpu/cpu.c (LGPL-2.1-or-later), pinned revision
 * 490f6d5dcbb2a5047345b8af88d114bbcaad69a8, whose notice follows.
 *
 * WoW64 CPU support
 *
 * Copyright 2021 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <emmintrin.h>
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "rtlsupportapi.h"
#include "wine/unixlib.h"
#include "wine/debug.h"
#include "wowprospero.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);

NTSTATUS WINAPI Wow64SystemServiceEx( UINT num, UINT *args );
NTSTATUS WINAPI Wow64RaiseException( int code, EXCEPTION_RECORD *rec );

/* Two never-executed guest addresses; the DBT stops when EIP reaches them. */
static BYTE *bop_page;
static NTSTATUS (WINAPI *unix_call_dispatcher)( unixlib_handle_t, unsigned int, void * );

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        LdrDisableThreadCalloutsForDll( inst );
        if (__wine_init_unix_call()) return FALSE;
    }
    return TRUE;
}

static WOW64_CPURESERVED *get_cpu(void)
{
    return NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];
}

static I386_CONTEXT *get_context( WOW64_CPURESERVED *cpu )
{
    return (I386_CONTEXT *)(cpu + 1);
}

static UINT get_teb32(void)
{
    return PtrToUlong( (BYTE *)NtCurrentTeb() + NtCurrentTeb()->WowTebOffset );
}

NTSTATUS WINAPI BTCpuProcessInit(void)
{
    HMODULE module;
    UNICODE_STRING str = RTL_CONSTANT_STRING( L"ntdll.dll" );
    void **dispatcher;
    SIZE_T size = 0x1000;
    ULONG old_prot;
    NTSTATUS status;
    void *page = NULL;

    LdrGetDllHandle( NULL, 0, &str, &module );
    dispatcher = RtlFindExportedRoutineByName( module, "__wine_unix_call_dispatcher" );
    if (!dispatcher) return STATUS_ENTRYPOINT_NOT_FOUND;
    unix_call_dispatcher = *dispatcher;

    /* The BOP addresses are stored as 32-bit values by wow64 and ntdll. */
    status = NtAllocateVirtualMemory( GetCurrentProcess(), &page, 0x7fffffff, &size,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    if (status) return status;
    bop_page = page;
    bop_page[0] = 0xcc;  /* int3: never executed, identifies the syscall BOP */
    bop_page[16] = 0xcc; /* Unix-call BOP */
    NtProtectVirtualMemory( GetCurrentProcess(), &page, &size, PAGE_EXECUTE_READ, &old_prot );

    status = WINE_UNIX_CALL( pw_wow_process_init, NULL );
    TRACE( "bop %p status %#lx\n", bop_page, status );
    return status;
}

void WINAPI BTCpuThreadInit(void)
{
}

void WINAPI BTCpuThreadTerm( HANDLE thread, LONG status )
{
    if (thread == GetCurrentThread() || !thread) WINE_UNIX_CALL( pw_wow_thread_term, NULL );
}

void * WINAPI BTCpuGetBopCode(void)
{
    return bop_page;
}

void * WINAPI __wine_get_unix_opcode(void)
{
    return bop_page + 16;
}

BOOLEAN WINAPI BTCpuIsProcessorFeaturePresent( UINT feature )
{
    /* The DBT publishes a conservative i386 profile; features it does not
     * translate must not be advertised to the guest. */
    switch (feature)
    {
    case PF_FLOATING_POINT_PRECISION_ERRATA:
    case PF_FLOATING_POINT_EMULATED:
        return FALSE;
    case PF_COMPARE_EXCHANGE_DOUBLE:
    case PF_MMX_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI64_INSTRUCTIONS_AVAILABLE:
    case PF_RDTSC_INSTRUCTION_AVAILABLE:
    case PF_NX_ENABLED:
        return RtlIsProcessorFeaturePresent( feature );
    default:
        return FALSE;
    }
}

NTSTATUS WINAPI BTCpuGetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return RtlWow64GetThreadContext( thread, ctx );
}

NTSTATUS WINAPI BTCpuSetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return RtlWow64SetThreadContext( thread, ctx );
}

NTSTATUS WINAPI BTCpuResetToConsistentState( EXCEPTION_POINTERS *ptrs )
{
    /* Guest state is only ever live inside the Unix library, which syncs it
     * back to the canonical context before returning; a 64-bit exception
     * never interrupts 32-bit execution. */
    return STATUS_SUCCESS;
}

static void flush( const void *addr, SIZE_T size )
{
    struct pw_wow_flush_params params = { (ULONG_PTR)addr, size };
    WINE_UNIX_CALL( pw_wow_flush, &params );
}

void WINAPI BTCpuFlushInstructionCache2( const void *addr, SIZE_T size )
{
    flush( addr, size );
}

void WINAPI BTCpuFlushInstructionCacheHeavy( const void *addr, SIZE_T size )
{
    flush( addr, size );
}

void WINAPI BTCpuNotifyMemoryFree( void *addr, SIZE_T size, ULONG type, BOOL is_after, NTSTATUS status )
{
    if (is_after && !status) flush( addr, size );
}

void WINAPI BTCpuNotifyMemoryProtect( void *addr, SIZE_T size, ULONG prot, BOOL is_after, NTSTATUS status )
{
    if (is_after && !status) flush( addr, size );
}

/* Wine tells the backend only the address of a view being unmapped, and a
 * flush with no size discards every thread's translations. A 32-bit DXVK
 * maps and unmaps windows of its texture memory thousands of times while a
 * game loads (Half-Life 2: about 12,000 times for its first map), so the
 * extent of each view is taken just before the unmap and only that range is
 * flushed after it. A view whose extent could not be kept is still flushed
 * whole. */
struct pending_unmap
{
    ULONG_PTR thread, address, base;
    SIZE_T size;
    ULONGLONG serial;
};
static struct pending_unmap pending_unmaps[64];
static RTL_SRWLOCK pending_lock = RTL_SRWLOCK_INIT;
static unsigned int pending_count;
static ULONGLONG pending_serial;
static BOOL pending_fallback;

/* A failed query anywhere in the walk means the complete extent is unknown.
 * In particular, never report a prefix of a multi-region mapped view. */
static SIZE_T view_extent( void *addr, ULONG_PTR *base )
{
    MEMORY_BASIC_INFORMATION info;
    ULONG_PTR start, end, region, next;

    if (NtQueryVirtualMemory( GetCurrentProcess(), addr, MemoryBasicInformation, &info, sizeof(info), NULL ) ||
        info.State == MEM_FREE || (info.Type != MEM_MAPPED && info.Type != MEM_IMAGE))
        return 0;
    start = end = (ULONG_PTR)info.AllocationBase;
    for (;;)
    {
        if (NtQueryVirtualMemory( GetCurrentProcess(), (void *)end, MemoryBasicInformation,
                                  &info, sizeof(info), NULL )) return 0;
        if (info.State == MEM_FREE || (ULONG_PTR)info.AllocationBase != start) break;
        region = (ULONG_PTR)info.BaseAddress;
        if (region > end || !info.RegionSize || info.RegionSize > ~(ULONG_PTR)0 - region) return 0;
        next = region + info.RegionSize;
        if (next <= end) return 0;
        end = next;
    }
    if (end == start) return 0;
    *base = start;
    return end - start;
}

void WINAPI BTCpuNotifyUnmapViewOfSection( void *addr, BOOL is_after, NTSTATUS status )
{
    ULONG_PTR at = (ULONG_PTR)addr, thread = (ULONG_PTR)NtCurrentTeb()->ClientId.UniqueThread, base = 0;
    SIZE_T size = 0;
    ULONGLONG serial = 0;
    unsigned int i, slot = ARRAY_SIZE(pending_unmaps);

    if (!is_after)
    {
        /* Reserve before querying: a nested notification must be newer even
         * if the query itself permits a user callback. Failed queries also
         * need a record, so their after notification cannot consume an older
         * successful query for the same thread/address. */
        RtlAcquireSRWLockExclusive( &pending_lock );
        pending_count++;
        if (!pending_fallback && thread)
        {
            for (i = 0; i < ARRAY_SIZE(pending_unmaps); i++)
                if (!pending_unmaps[i].thread) { slot = i; break; }
            if (slot != ARRAY_SIZE(pending_unmaps) && (serial = ++pending_serial))
                pending_unmaps[slot] = (struct pending_unmap){ thread, at, 0, 0, serial };
            else pending_fallback = TRUE;
        }
        else pending_fallback = TRUE;
        RtlReleaseSRWLockExclusive( &pending_lock );
        if (!serial) return;
        size = view_extent( addr, &base );
        RtlAcquireSRWLockExclusive( &pending_lock );
        if (!pending_fallback && pending_unmaps[slot].serial == serial)
        {
            pending_unmaps[slot].base = base;
            pending_unmaps[slot].size = size;
        }
        RtlReleaseSRWLockExclusive( &pending_lock );
        return;
    }
    RtlAcquireSRWLockExclusive( &pending_lock );
    if (pending_count && !pending_fallback)
    {
        /* Notifications nest synchronously on each thread. Match its newest
         * record, rather than any other thread's overlapping address range. */
        for (i = 0; i < ARRAY_SIZE(pending_unmaps); i++)
            if (pending_unmaps[i].thread == thread && pending_unmaps[i].serial > serial)
            {
                serial = pending_unmaps[i].serial;
                slot = i;
            }
        if (slot != ARRAY_SIZE(pending_unmaps) && pending_unmaps[slot].address == at)
        {
            base = pending_unmaps[slot].base;
            size = pending_unmaps[slot].size;
            pending_unmaps[slot].thread = 0;
        }
        else pending_fallback = TRUE;
    }
    if (pending_count) pending_count--;
    /* Saturation or an unpaired callback makes all active pairs uncertain.
     * Flush wholly until they drain; only then reuse the bounded bank. */
    if (!pending_count)
    {
        if (pending_fallback) memset( pending_unmaps, 0, sizeof(pending_unmaps) );
        pending_fallback = FALSE;
        pending_serial = 0;
    }
    RtlReleaseSRWLockExclusive( &pending_lock );
    if (status) return;
    if (size) flush( (void *)base, size );
    else flush( addr, 0 );
}

static void raise_guest_exception( I386_CONTEXT *ctx, DWORD code, UINT address, UINT write )
{
    static LONG logged;
    EXCEPTION_RECORD rec = { 0 };

    /* The first exceptions a process raises into the guest, for the log: a
     * storm of faults usually starts from one of them. */
    if (InterlockedIncrement( &logged ) <= 16)
    {
        MEMORY_BASIC_INFORMATION info;
        const UINT *stack = ULongToPtr( ctx->Esp );

        ERR( "guest exception %#lx at eip %#lx esp %#lx address %#x write %u\n",
             code, ctx->Eip, ctx->Esp, address, write );
        /* The top of the i386 stack, for the callers' return addresses. */
        if (!NtQueryVirtualMemory( GetCurrentProcess(), stack, MemoryBasicInformation, &info, sizeof(info), NULL ) &&
            info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (const char *)info.BaseAddress + info.RegionSize >= (const char *)(stack + 16))
            ERR( "guest stack %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                 stack[0], stack[1], stack[2], stack[3], stack[4], stack[5], stack[6], stack[7],
                 stack[8], stack[9], stack[10], stack[11], stack[12], stack[13], stack[14], stack[15] );
    }

    rec.ExceptionCode = code;
    rec.ExceptionAddress = ULongToPtr( ctx->Eip );
    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        rec.NumberParameters = 2;
        rec.ExceptionInformation[0] = write ? EXCEPTION_WRITE_FAULT : EXCEPTION_READ_FAULT;
        rec.ExceptionInformation[1] = address;
    }
    Wow64RaiseException( -1, &rec );
}

/* The FXSAVE image between the thread's hardware state and the context, on
 * every entry and exit of translated code: that is, on every system and
 * Unix call the guest makes. ntdll's memcpy copies a byte at a time, which
 * made these two copies a fifth of the CPU time of an OpenGL game; the
 * context's image is not 16-byte aligned, so unaligned SSE moves. */
static inline void copy_fxsave( void *dst, const void *src )
{
    __m128i *d = dst;
    const __m128i *s = src;

    for (unsigned int i = 0; i < sizeof(XSAVE_FORMAT) / sizeof(__m128i); i++)
        _mm_storeu_si128( d + i, _mm_loadu_si128( s + i ) );
}

/* In cpu->Flags: the guest's x87 and SSE state is in the context's FXSAVE
 * image only, not in this thread's hardware state (see BTCpuSimulate). Wine's
 * wow64 saves and restores the flags around a user callback, as this needs. */
#define PW_FP_IN_CONTEXT 0x8000

void WINAPI BTCpuSimulate(void)
{
    WOW64_CPURESERVED *cpu = get_cpu();
    I386_CONTEXT *ctx = get_context( cpu );
    struct pw_wow_run_params params;
    DECLSPEC_ALIGN(16) XSAVE_FORMAT fp;
    NTSTATUS status;
    UINT *stack;

    C_ASSERT( sizeof(ctx->ExtendedRegisters) == sizeof(fp) );

    for (;;)
    {
        /* The Unix side reloads the complete context on every entry, which
         * is what RESET_STATE requests; clear it so that a flag left by
         * RtlWow64SetThreadContext (e.g. the initial thread context) is not
         * mistaken for a context replaced by the next system call. */
        cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
        params.context = (ULONG_PTR)ctx;
        params.teb32 = get_teb32();
        params.bop = PtrToUlong( bop_page );
        params.unix_bop = PtrToUlong( bop_page + 16 );
        params.reason = 0;
        /* The guest's x87 and SSE state is this thread's hardware state
         * while the guest is out for a system call, as with wow64cpu: Wine
         * keeps a wow64 thread's 32-bit FP context there (frame->xsave), so
         * what NtContinue and SetThreadContext restore reaches it and
         * GetThreadContext and exception dispatch read it. A Unix call
         * (OpenGL, Vulkan, sockets) never reaches a thread context, so for
         * one the state stays in the context's image instead: saving and
         * restoring the hardware around each of them cost an OpenGL game
         * about a sixth of its time, as it makes a Unix call per GL call.
         * The Unix side runs on the FXSAVE image in between. */
        if (!(cpu->Flags & PW_FP_IN_CONTEXT))
        {
            __asm__ volatile( "fxsave %0" : "=m" (fp) );
            copy_fxsave( ctx->ExtendedRegisters, &fp );
        }
        status = WINE_UNIX_CALL( pw_wow_run, &params );
        if (!status && params.reason == PW_WOW_UNIXCALL)
            cpu->Flags |= PW_FP_IN_CONTEXT;
        else
        {
            copy_fxsave( &fp, ctx->ExtendedRegisters );
            __asm__ volatile( "fxrstor %0" : : "m" (fp) );
            cpu->Flags &= ~PW_FP_IN_CONTEXT;
        }
        if (status)
        {
            /* A host fault inside translated code: Wine unwound the Unix
             * call and returned the exception code. */
            ERR( "host exception %#lx in translated code near eip %#lx\n", status, ctx->Eip );
            WINE_UNIX_CALL( pw_wow_dump, NULL );
            raise_guest_exception( ctx, status, 0, 0 );
            continue;
        }
        stack = ULongToPtr( ctx->Esp );
        /* A guest stack pointer in the first 64 KiB is never a valid stack:
         * name where the guest left it, once per process. */
        if (ctx->Esp < 0x10000)
        {
            static LONG warned;
            if (!InterlockedExchange( &warned, 1 ))
                ERR( "guest esp %#lx at eip %#lx after reason %u (eax %#lx ebp %#lx)\n",
                     ctx->Esp, ctx->Eip, params.reason, ctx->Eax, ctx->Ebp );
        }
        switch (params.reason)
        {
        case PW_WOW_SYSCALL:
        {
            UINT num = ctx->Eax;

            /* cf. syscall_32to64: return address of the stub's call, then
             * the caller's return address, then the arguments. */
            ctx->Eip = stack[0];
            ctx->Esp += 4;
            status = Wow64SystemServiceEx( num, stack + 2 );
            if (cpu->Flags & WOW64_CPURESERVED_FLAG_RESET_STATE)
                cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
            else
                ctx->Eax = status;
            break;
        }
        case PW_WOW_UNIXCALL:
        {
            /* cf. unix_call_32to64: handle (8 bytes), code, args. */
            unixlib_handle_t handle = *(UINT64 *)(stack + 1);
            UINT code = stack[3];
            void *args = ULongToPtr( stack[4] );

            ctx->Eip = stack[0];
            ctx->Esp += 20;
            status = unix_call_dispatcher( handle, code, args );
            if (cpu->Flags & WOW64_CPURESERVED_FLAG_RESET_STATE)
                cpu->Flags &= ~WOW64_CPURESERVED_FLAG_RESET_STATE;
            else
                ctx->Eax = status;
            break;
        }
        case PW_WOW_FAULT:
            raise_guest_exception( ctx, EXCEPTION_ACCESS_VIOLATION,
                                   params.fault_address, params.fault_write );
            break;
        case PW_WOW_UNSUPPORTED:
        {
            const BYTE *code = ULongToPtr( ctx->Eip );
            ERR( "untranslatable instruction at eip %#lx: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 ctx->Eip, code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7] );
        }
            raise_guest_exception( ctx, EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0 );
            break;
        case PW_WOW_X87_TRAP:
            raise_guest_exception( ctx, EXCEPTION_FLT_INVALID_OPERATION, 0, 0 );
            break;
        default:
            ERR( "DBT error %d at eip %#lx\n", params.status, ctx->Eip );
            /* End the process the way ExitProcess does: the other threads
             * first, which marks the process as exiting, so the call for
             * itself leaves through exit() and the host's exit handlers run
             * (a title restarts into its launcher from one). Terminating
             * itself straight away is abort_process, which is _exit(). */
            NtTerminateProcess( 0, STATUS_INTERNAL_ERROR );
            NtTerminateProcess( GetCurrentProcess(), STATUS_INTERNAL_ERROR );
        }
    }
}
