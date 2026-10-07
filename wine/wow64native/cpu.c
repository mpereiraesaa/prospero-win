/*
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

#include <stdarg.h>
#include <cpuid.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "rtlsupportapi.h"
#include "wine/asm.h"
#include "wine/debug.h"
#include "wine/unixlib.h"
#include "wow64native.h"

WINE_DEFAULT_DEBUG_CHANNEL(wow);

#pragma pack(push,1)
struct thunk_32to64
{
    BYTE  ljmp;   /* jump far, absolute indirect */
    BYTE  modrm;  /* address=disp32, opcode=5 */
    DWORD op;
    DWORD addr;
    WORD  cs;
};
struct thunk_opcodes
{
    struct thunk_32to64 syscall_thunk;
    struct thunk_32to64 unix_thunk;
};
#pragma pack(pop)

static BYTE DECLSPEC_ALIGN(4096) code_buffer[0x1000];

UINT cs32_sel = PW_NATIVE_CS32;
UINT ss32_sel = PW_NATIVE_SS32;

static USHORT cs64_sel = PW_NATIVE_CS64;
static USHORT ds64_sel = PW_NATIVE_SS32;
static USHORT fs32_sel;
static NTSTATUS process_status = STATUS_NOT_SUPPORTED;
void *pw_native_sysarch;
UINT pw_native_avx;

/* FXSAVE covers x87 and XMM. Preserve all sixteen upper YMM halves too
 * when AVX is enabled by the OS. The supported xstate set is checked at
 * initialization; unknown components cannot silently cross a host call. */
#define PW_SAVE_YMM_HIGH(off) \
    "cmpl $0,pw_native_avx(%rip)\n\tje 8f\n\t" \
    "vextractf128 $1,%ymm0," off "+0x00(%rsp)\n\t" \
    "vextractf128 $1,%ymm1," off "+0x10(%rsp)\n\t" \
    "vextractf128 $1,%ymm2," off "+0x20(%rsp)\n\t" \
    "vextractf128 $1,%ymm3," off "+0x30(%rsp)\n\t" \
    "vextractf128 $1,%ymm4," off "+0x40(%rsp)\n\t" \
    "vextractf128 $1,%ymm5," off "+0x50(%rsp)\n\t" \
    "vextractf128 $1,%ymm6," off "+0x60(%rsp)\n\t" \
    "vextractf128 $1,%ymm7," off "+0x70(%rsp)\n\t" \
    "vextractf128 $1,%ymm8," off "+0x80(%rsp)\n\t" \
    "vextractf128 $1,%ymm9," off "+0x90(%rsp)\n\t" \
    "vextractf128 $1,%ymm10," off "+0xa0(%rsp)\n\t" \
    "vextractf128 $1,%ymm11," off "+0xb0(%rsp)\n\t" \
    "vextractf128 $1,%ymm12," off "+0xc0(%rsp)\n\t" \
    "vextractf128 $1,%ymm13," off "+0xd0(%rsp)\n\t" \
    "vextractf128 $1,%ymm14," off "+0xe0(%rsp)\n\t" \
    "vextractf128 $1,%ymm15," off "+0xf0(%rsp)\n\t8:\n\t"
#define PW_RESTORE_YMM_HIGH(off) \
    "cmpl $0,pw_native_avx(%rip)\n\tje 8f\n\t" \
    "vinsertf128 $1," off "+0x00(%rsp),%ymm0,%ymm0\n\t" \
    "vinsertf128 $1," off "+0x10(%rsp),%ymm1,%ymm1\n\t" \
    "vinsertf128 $1," off "+0x20(%rsp),%ymm2,%ymm2\n\t" \
    "vinsertf128 $1," off "+0x30(%rsp),%ymm3,%ymm3\n\t" \
    "vinsertf128 $1," off "+0x40(%rsp),%ymm4,%ymm4\n\t" \
    "vinsertf128 $1," off "+0x50(%rsp),%ymm5,%ymm5\n\t" \
    "vinsertf128 $1," off "+0x60(%rsp),%ymm6,%ymm6\n\t" \
    "vinsertf128 $1," off "+0x70(%rsp),%ymm7,%ymm7\n\t" \
    "vinsertf128 $1," off "+0x80(%rsp),%ymm8,%ymm8\n\t" \
    "vinsertf128 $1," off "+0x90(%rsp),%ymm9,%ymm9\n\t" \
    "vinsertf128 $1," off "+0xa0(%rsp),%ymm10,%ymm10\n\t" \
    "vinsertf128 $1," off "+0xb0(%rsp),%ymm11,%ymm11\n\t" \
    "vinsertf128 $1," off "+0xc0(%rsp),%ymm12,%ymm12\n\t" \
    "vinsertf128 $1," off "+0xd0(%rsp),%ymm13,%ymm13\n\t" \
    "vinsertf128 $1," off "+0xe0(%rsp),%ymm14,%ymm14\n\t" \
    "vinsertf128 $1," off "+0xf0(%rsp),%ymm15,%ymm15\n\t8:\n\t"

static NTSTATUS init_xstate(void)
{
    unsigned int eax, ebx, ecx, edx, low, high;
    pw_native_avx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return STATUS_NOT_SUPPORTED;
    if (!(ecx & bit_OSXSAVE)) return STATUS_SUCCESS; /* x87/SSE only */
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    if (high || (low & ~7u) || (low & 3u) != 3u) return STATUS_NOT_SUPPORTED;
    if (low & 4u)
    {
        if (!(ecx & bit_AVX)) return STATUS_NOT_SUPPORTED;
        pw_native_avx = 1;
    }
    return STATUS_SUCCESS;
}

C_ASSERT(offsetof(struct pw_native_thread_state, host_fs) == 0);
C_ASSERT(offsetof(struct pw_native_thread_state, guest_fs) == 8);
C_ASSERT(offsetof(struct pw_native_thread_state, profile) == 24);
C_ASSERT(offsetof(struct pw_native_profile, host_calls) == 0);
C_ASSERT(offsetof(struct pw_native_profile, guest_calls) == 8);
C_ASSERT(offsetof(struct pw_native_profile, host_sysarch_ticks) == 16);
C_ASSERT(offsetof(struct pw_native_profile, guest_sysarch_ticks) == 24);
C_ASSERT(offsetof(struct pw_native_profile, unix_calls) == 32);
C_ASSERT(offsetof(struct pw_native_profile, syscall_calls) == 40);
C_ASSERT(offsetof(struct pw_native_profile, last_tsc) == 48);
C_ASSERT(offsetof(struct pw_native_profile, report_proc) == 64);
C_ASSERT(sizeof(struct pw_native_profile) == 72);

/* No C or TLS lookup before host FS. Preserve guest flags/registers; the
 * profile pointer is checked on every entry and is NULL by default. */
#define PW_PROFILE_ENTRY(offset) \
    "pushfq\n\t" \
    "pushq %rax\n\t" \
    "movq 24(%r15),%rax\n\t" \
    "testq %rax,%rax\n\t" \
    "jz 9f\n\t" \
    "incq " offset "(%rax)\n\t9:\n\t" \
    "popq %rax\n\t" \
    "popfq\n\t"

/* These wrappers use r15, which compatibility-mode code cannot address.
 * Everything touched below is an explicit address or register: no C, CRT,
 * emulated TLS, or Windows-to-Unix dispatcher runs with guest FS active.
 * sysarch itself uses the Unix register ABI. Signal/exception xstate support
 * and the platform fault bridge still gate native execution. */
__ASM_GLOBAL_FUNC(pw_native_restore_host_fs,
                  ".seh_endprologue\n\t"
                  "leaq 0(%r15),%r11\n\t"
                  "jmp pw_native_switch_fs")
__ASM_GLOBAL_FUNC(pw_native_restore_guest_fs,
                  ".seh_endprologue\n\t"
                  "leaq 8(%r15),%r11\n\t"
                  "jmp pw_native_switch_fs")
__ASM_GLOBAL_FUNC(pw_native_switch_fs,
                  "pushfq\n\t"
                  "pushq %rax\n\t"
                  "pushq %rcx\n\t"
                  "pushq %rdx\n\t"
                  "pushq %rsi\n\t"
                  "pushq %rdi\n\t"
                  "pushq %r8\n\t"
                  "pushq %r9\n\t"
                  "pushq %r10\n\t"
                  "pushq %r11\n\t"
                  "subq $0x308,%rsp\n\t"
                  ".seh_stackalloc 0x358\n\t"
                  ".seh_savereg %rdi,0x328\n\t"
                  ".seh_savereg %rsi,0x330\n\t"
                  "fxsave64 (%rsp)\n\t"
                  ".seh_savexmm %xmm6,0x100\n\t"
                  ".seh_savexmm %xmm7,0x110\n\t"
                  ".seh_savexmm %xmm8,0x120\n\t"
                  ".seh_savexmm %xmm9,0x130\n\t"
                  ".seh_savexmm %xmm10,0x140\n\t"
                  ".seh_savexmm %xmm11,0x150\n\t"
                  ".seh_savexmm %xmm12,0x160\n\t"
                  ".seh_savexmm %xmm13,0x170\n\t"
                  ".seh_savexmm %xmm14,0x180\n\t"
                  ".seh_savexmm %xmm15,0x190\n\t"
                  ".seh_endprologue\n\t"
                  "cld\n\t" /* Unix ABI requires DF clear; saved flags restore it on return. */
                  PW_SAVE_YMM_HIGH("0x200")
                  "movq %r11,%rsi\n\t"
                  "movl $129,%edi\n\t" /* AMD64_SET_FSBASE */
                  "movq 24(%r15),%rax\n\t"
                  "testq %rax,%rax\n\t"
                  "jz 9f\n\t"
                  "lfence\n\t"
                  "rdtsc\n\t"
                  "shlq $32,%rdx\n\t"
                  "orq %rdx,%rax\n\t"
                  "movq %rax,0x300(%rsp)\n\t9:\n\t"
                  "call *pw_native_sysarch(%rip)\n\t"
                  "testl %eax,%eax\n\t"
                  "jnz .Lnative_fs_failed\n\t"
                  "movq 24(%r15),%r10\n\t"
                  "testq %r10,%r10\n\t"
                  "jz 9f\n\t"
                  "lfence\n\t"
                  "rdtsc\n\t"
                  "shlq $32,%rdx\n\t"
                  "orq %rdx,%rax\n\t"
                  "movq %rax,48(%r10)\n\t"
                  "subq 0x300(%rsp),%rax\n\t"
                  "cmpq %r15,0x308(%rsp)\n\t"
                  "je .Lnative_profile_host\n\t"
                  "incq 8(%r10)\n\t"
                  "addq %rax,24(%r10)\n\t"
                  "jmp 9f\n\t"
                  ".Lnative_profile_host:\n\t"
                  "incq 0(%r10)\n\t"
                  "addq %rax,16(%r10)\n\t"
                  "movl 0(%r10),%eax\n\t"
                  "andl $0x3ffff,%eax\n\t"
                  "cmpl $1,%eax\n\t"
                  "jne 9f\n\t"
                  "movq %r15,%rdi\n\t"
                  "call *64(%r10)\n\t9:\n\t"
                  "fxrstor64 (%rsp)\n\t"
                  PW_RESTORE_YMM_HIGH("0x200")
                  "addq $0x308,%rsp\n\t"
                  "popq %r11\n\t"
                  "popq %r10\n\t"
                  "popq %r9\n\t"
                  "popq %r8\n\t"
                  "popq %rdi\n\t"
                  "popq %rsi\n\t"
                  "popq %rdx\n\t"
                  "popq %rcx\n\t"
                  "popq %rax\n\t"
                  "popfq\n\t"
                  "ret\n\t"
                  ".Lnative_fs_failed:\n\t"
                  /* Recovery must restore host FS before any Wine entry.
                   * Never resume guest code after a failed FS change. */
                  "leaq 0(%r15),%rsi\n\t"
                  "movl $129,%edi\n\t"
                  "call *pw_native_sysarch(%rip)\n\t"
                  "testl %eax,%eax\n\t"
                  "jnz .Lnative_fs_unrecoverable\n\t"
                  "movq $-1,%rcx\n\t"
                  "movl $0xc0000001,%edx\n\t"
                  "call " __ASM_NAME("NtTerminateProcess") "\n\t"
                  ".Lnative_fs_unrecoverable:\n\t"
                  "ud2")


BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        LdrDisableThreadCalloutsForDll( inst );
        if (__wine_init_unix_call()) return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *           fpux_to_fpu
 *
 * Build a standard i386 FPU context from an extended one.
 */
static void fpux_to_fpu( I386_FLOATING_SAVE_AREA *fpu, const XMM_SAVE_AREA32 *fpux )
{
    unsigned int i, tag, stack_top;

    fpu->ControlWord   = fpux->ControlWord;
    fpu->StatusWord    = fpux->StatusWord;
    fpu->ErrorOffset   = fpux->ErrorOffset;
    fpu->ErrorSelector = fpux->ErrorSelector | (fpux->ErrorOpcode << 16);
    fpu->DataOffset    = fpux->DataOffset;
    fpu->DataSelector  = fpux->DataSelector;
    fpu->Cr0NpxState   = fpux->StatusWord | 0xffff0000;

    stack_top = (fpux->StatusWord >> 11) & 7;
    fpu->TagWord = 0xffff0000;
    for (i = 0; i < 8; i++)
    {
        memcpy( &fpu->RegisterArea[10 * i], &fpux->FloatRegisters[i], 10 );
        if (!(fpux->TagWord & (1 << i))) tag = 3;  /* empty */
        else
        {
            const M128A *reg = &fpux->FloatRegisters[(i - stack_top) & 7];
            if ((reg->High & 0x7fff) == 0x7fff)  /* exponent all ones */
            {
                tag = 2;  /* special */
            }
            else if (!(reg->High & 0x7fff))  /* exponent all zeroes */
            {
                if (reg->Low) tag = 2;  /* special */
                else tag = 1;  /* zero */
            }
            else
            {
                if (reg->Low >> 63) tag = 0;  /* valid */
                else tag = 2;  /* special */
            }
        }
        fpu->TagWord |= tag << (2 * i);
    }
}

/**********************************************************************
 *           copy_context_64to32
 *
 * Copy a 64-bit context corresponding to an exception happening in 32-bit mode
 * into the corresponding 32-bit context.
 */
static void copy_context_64to32( I386_CONTEXT *ctx32, DWORD flags, AMD64_CONTEXT *ctx64 )
{
    ctx32->ContextFlags = flags;
    flags &= ~CONTEXT_i386;
    if (flags & CONTEXT_I386_INTEGER)
    {
        ctx32->Eax = ctx64->Rax;
        ctx32->Ebx = ctx64->Rbx;
        ctx32->Ecx = ctx64->Rcx;
        ctx32->Edx = ctx64->Rdx;
        ctx32->Esi = ctx64->Rsi;
        ctx32->Edi = ctx64->Rdi;
    }
    if (flags & CONTEXT_I386_CONTROL)
    {
        ctx32->Esp    = ctx64->Rsp;
        ctx32->Ebp    = ctx64->Rbp;
        ctx32->Eip    = ctx64->Rip;
        ctx32->EFlags = ctx64->EFlags;
        ctx32->SegCs  = ctx64->SegCs;
        ctx32->SegSs  = ctx64->SegSs;
    }
    if (flags & CONTEXT_I386_SEGMENTS)
    {
        ctx32->SegDs = ctx64->SegDs;
        ctx32->SegEs = ctx64->SegEs;
        ctx32->SegFs = fs32_sel;
        ctx32->SegGs = ds64_sel;
    }
    if (flags & CONTEXT_I386_DEBUG_REGISTERS)
    {
        ctx32->Dr0 = ctx64->Dr0;
        ctx32->Dr1 = ctx64->Dr1;
        ctx32->Dr2 = ctx64->Dr2;
        ctx32->Dr3 = ctx64->Dr3;
        ctx32->Dr6 = ctx64->Dr6;
        ctx32->Dr7 = ctx64->Dr7;
    }
    if (flags & CONTEXT_I386_FLOATING_POINT)
    {
        fpux_to_fpu( &ctx32->FloatSave, &ctx64->FltSave );
    }
    if (flags & CONTEXT_I386_EXTENDED_REGISTERS)
    {
        *(XSAVE_FORMAT *)ctx32->ExtendedRegisters = ctx64->FltSave;
    }
    /* FIXME: xstate */
}


/**********************************************************************
 *           syscall_32to64
 *
 * Execute a 64-bit syscall from 32-bit code, then return to 32-bit.
 */
extern void WINAPI syscall_32to64(void);
__ASM_GLOBAL_FUNC( syscall_32to64,
                   /* cf. BTCpuSimulate prolog */
                   ".seh_pushreg %rbp\n\t"
                   ".seh_pushreg %rbx\n\t"
                   ".seh_pushreg %rsi\n\t"
                   ".seh_pushreg %rdi\n\t"
                   ".seh_pushreg %r15\n\t"
                   ".seh_stackalloc 0x340\n\t"
                   ".seh_endprologue\n\t"
                   "xchgq %r14,%rsp\n\t"
                   "movl %edi,0x9c(%r13)\n\t"   /* context->Edi */
                   "movl %esi,0xa0(%r13)\n\t"   /* context->Esi */
                   "movl %ebx,0xa4(%r13)\n\t"   /* context->Ebx */
                   "movl %ebp,0xb4(%r13)\n\t"   /* context->Ebp */
                   "movl (%r14),%edx\n\t"
                   "movl %edx,0xb8(%r13)\n\t"   /* context->Eip */
                   "movl cs32_sel(%rip),%edx\n\t"
                   "movl %edx,0xbc(%r13)\n\t"   /* context->SegCs */
                   "pushfq\n\t"
                   "popq %rdx\n\t"
                   "movl %edx,0xc0(%r13)\n\t"   /* context->EFlags */
                   "leaq 4(%r14),%rdx\n\t"
                   "movl %edx,0xc4(%r13)\n\t"   /* context->Esp */
                   PW_PROFILE_ENTRY("40")
                   "call pw_native_restore_host_fs\n\t"
                   "movq %rax,%rcx\n\t"         /* syscall number */
                   "leaq 8(%r14),%rdx\n\t"      /* parameters */
                   "call " __ASM_NAME("Wow64SystemServiceEx") "\n\t"
                   "movl %eax,0xb0(%r13)\n\t"   /* context->Eax */

                   "syscall_32to64_return:\n\t"
                   "movl 0x9c(%r13),%edi\n\t"   /* context->Edi */
                   "movl 0xa0(%r13),%esi\n\t"   /* context->Esi */
                   "movl 0xa4(%r13),%ebx\n\t"   /* context->Ebx */
                   "movl 0xb4(%r13),%ebp\n\t"   /* context->Ebp */
                   "btrl $0,-4(%r13)\n\t"       /* cpu->Flags & WOW64_CPURESERVED_FLAG_RESET_STATE */
                   "jc .Lsyscall_32to64_return\n\t"
                   "movl 0xb8(%r13),%edx\n\t"   /* context->Eip */
                   "movl %edx,(%rsp)\n\t"
                   "movl 0xbc(%r13),%edx\n\t"   /* context->SegCs */
                   "movl %edx,4(%rsp)\n\t"
                   "movl 0xc4(%r13),%r14d\n\t"  /* context->Esp */
                   "call pw_native_restore_guest_fs\n\t"
                   "xchgq %r14,%rsp\n\t"
                   "ljmp *(%r14)\n"
                   ".Lsyscall_32to64_return:\n\t"
                   "movq %rsp,%r14\n\t"
                   "movl 0xa8(%r13),%edx\n\t"   /* context->Edx */
                   "movl 0xac(%r13),%ecx\n\t"   /* context->Ecx */
                   "movl 0xc8(%r13),%eax\n\t"   /* context->SegSs */
                   "movq %rax,0x20(%rsp)\n\t"
                   "mov %ax,%ds\n\t"
                   "mov %ax,%es\n\t"
                   "mov 0x90(%r13),%fs\n\t"     /* context->SegFs */
                   "movl 0xc4(%r13),%eax\n\t"   /* context->Esp */
                   "movq %rax,0x18(%rsp)\n\t"
                   "movl 0xc0(%r13),%eax\n\t"   /* context->EFlags */
                   "movq %rax,0x10(%rsp)\n\t"
                   "movl 0xbc(%r13),%eax\n\t"   /* context->SegCs */
                   "movq %rax,0x8(%rsp)\n\t"
                   "movl 0xb8(%r13),%eax\n\t"   /* context->Eip */
                   "movq %rax,(%rsp)\n\t"
                   "movl 0xb0(%r13),%eax\n\t"   /* context->Eax */
                   "call pw_native_restore_guest_fs\n\t"
                   "iretq" )


/**********************************************************************
 *           unix_call_32to64
 *
 * Execute a 64-bit Unix call from 32-bit code, then return to 32-bit.
 */
extern void WINAPI unix_call_32to64(void);
__ASM_GLOBAL_FUNC( unix_call_32to64,
                   /* cf. BTCpuSimulate prolog */
                   ".seh_pushreg %rbp\n\t"
                   ".seh_pushreg %rbx\n\t"
                   ".seh_pushreg %rsi\n\t"
                   ".seh_pushreg %rdi\n\t"
                   ".seh_pushreg %r15\n\t"
                   ".seh_stackalloc 0x340\n\t"
                   ".seh_endprologue\n\t"
                   "xchgq %r14,%rsp\n\t"
                   "movl %edi,0x9c(%r13)\n\t"   /* context->Edi */
                   "movl %esi,0xa0(%r13)\n\t"   /* context->Esi */
                   "movl %ebx,0xa4(%r13)\n\t"   /* context->Ebx */
                   "movl %ebp,0xb4(%r13)\n\t"   /* context->Ebp */
                   "movl (%r14),%edx\n\t"
                   "movl %edx,0xb8(%r13)\n\t"   /* context->Eip */
                   "movl cs32_sel(%rip),%edx\n\t"
                   "movl %edx,0xbc(%r13)\n\t"   /* context->SegCs */
                   "leaq 20(%r14),%rdx\n\t"
                   "movl %edx,0xc4(%r13)\n\t"   /* context->Esp */
                   PW_PROFILE_ENTRY("32")
                   "call pw_native_restore_host_fs\n\t"
                   "movq 4(%r14),%rcx\n\t"      /* handle */
                   "movl 12(%r14),%edx\n\t"     /* code */
                   "movl 16(%r14),%r8d\n\t"     /* args */
                   "callq *__wine_unix_call_dispatcher(%rip)\n\t"
                   "movl %eax,0xb0(%r13)\n\t"   /* context->Eax */
                   "btrl $0,-4(%r13)\n\t"       /* cpu->Flags & WOW64_CPURESERVED_FLAG_RESET_STATE */
                   "jc .Lsyscall_32to64_return\n\t"
                   "movl 0xb8(%r13),%edx\n\t"   /* context->Eip */
                   "movl %edx,(%rsp)\n\t"
                   "movl 0xbc(%r13),%edx\n\t"   /* context->SegCs */
                   "movl %edx,4(%rsp)\n\t"
                   "movl 0xc4(%r13),%r14d\n\t"  /* context->Esp */
                   "call pw_native_restore_guest_fs\n\t"
                   "xchgq %r14,%rsp\n\t"
                   "ljmp *(%r14)" )


/**********************************************************************
 *           BTCpuSimulate  (wow64cpu.@)
 */
__ASM_GLOBAL_FUNC( BTCpuSimulate,
                   "pushq %rbp\n\t"
                   ".seh_pushreg %rbp\n\t"
                   "pushq %rbx\n\t"
                   ".seh_pushreg %rbx\n\t"
                   "pushq %rsi\n\t"
                   ".seh_pushreg %rsi\n\t"
                   "pushq %rdi\n\t"
                   ".seh_pushreg %rdi\n\t"
                   "pushq %r15\n\t"
                   ".seh_pushreg %r15\n\t"
                   "subq $0x340,%rsp\n"
                   ".seh_stackalloc 0x340\n\t"
                   ".seh_endprologue\n\t"
                   "movq %gs:0x30,%r12\n\t"
                   "fxsave64 0x30(%rsp)\n\t"
                   PW_SAVE_YMM_HIGH("0x230")
                   "call " __ASM_NAME("pw_native_prepare_simulate") "\n\t"
                   "movq %rax,%r15\n\t"       /* Unix-owned per-thread transition state */
                   "fxrstor64 0x30(%rsp)\n\t"
                   PW_RESTORE_YMM_HIGH("0x230")
                   "movq 0x1488(%r12),%rcx\n\t" /* NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED] */
                   "leaq 4(%rcx),%r13\n\t"      /* cpu->Context */
                   "movl cs32_sel(%rip),%eax\n\t"
                   "movl %eax,0xbc(%r13)\n\t"   /* context->SegCs */
                   "movl ss32_sel(%rip),%eax\n\t"
                   "movl %eax,0xc8(%r13)\n\t"   /* context->SegSs */
                   "jmp syscall_32to64_return\n" )


/**********************************************************************
 *           BTCpuProcessInit  (wow64cpu.@)
 */
static NTSTATUS process_init(void)
{
    struct thunk_opcodes *thunk = (struct thunk_opcodes *)code_buffer;
    SIZE_T size = sizeof(*thunk);
    ULONG old_prot;
    CONTEXT context;
    struct pw_native_init_params native = { PW_NATIVE_ABI_VERSION, 0 };
    NTSTATUS status;
    HMODULE module;
    UNICODE_STRING str = RTL_CONSTANT_STRING( L"ntdll.dll" );
    NTSTATUS (WINAPI **p__wine_unix_call_dispatcher)( unixlib_handle_t, unsigned int, void * );
    WOW64INFO *wow64info = NtCurrentTeb()->TlsSlots[WOW64_TLS_WOW64INFO];

    if ((ULONG_PTR)syscall_32to64 >> 32 || (ULONG_PTR)unix_call_32to64 >> 32 ||
        (ULONG_PTR)code_buffer > 0x100000000ULL - sizeof(code_buffer))
    {
        ERR( "wow64cpu loaded above 4G, disabling\n" );
        return STATUS_INVALID_ADDRESS;
    }

    /* The module is selectable independently of the software backend. Do
     * not enter compatibility mode until its TLS/signal transition layer
     * has explicitly completed initialization. */
    status = init_xstate();
    if (status) return status;
    status = WINE_UNIX_CALL( pw_native_process_init, &native );
    if (status) return status;
    if (!native.transitions_ready) return STATUS_NOT_SUPPORTED;
    if (!native.fs_set_proc) return STATUS_ENTRYPOINT_NOT_FOUND;
    pw_native_sysarch = (void *)(ULONG_PTR)native.fs_set_proc;

    LdrGetDllHandle( NULL, 0, &str, &module );
    p__wine_unix_call_dispatcher = RtlFindExportedRoutineByName( module, "__wine_unix_call_dispatcher" );
    if (!p__wine_unix_call_dispatcher) return STATUS_ENTRYPOINT_NOT_FOUND;
    __wine_unix_call_dispatcher = *p__wine_unix_call_dispatcher;

    RtlCaptureContext( &context );
    if (context.SegCs != PW_NATIVE_CS64) return STATUS_NOT_SUPPORTED;

    /* Do not obtain compatibility selectors from a 64-bit thread context. */
    cs32_sel = PW_NATIVE_CS32;
    ss32_sel = PW_NATIVE_SS32;
    cs64_sel = PW_NATIVE_CS64;
    ds64_sel = PW_NATIVE_SS32;
    fs32_sel = native.fs32_selector;

    thunk->syscall_thunk.ljmp  = 0xff;
    thunk->syscall_thunk.modrm = 0x2d;
    thunk->syscall_thunk.op    = PtrToUlong( &thunk->syscall_thunk.addr );
    thunk->syscall_thunk.addr  = PtrToUlong( syscall_32to64 );
    thunk->syscall_thunk.cs    = cs64_sel;

    thunk->unix_thunk.ljmp  = 0xff;
    thunk->unix_thunk.modrm = 0x2d;
    thunk->unix_thunk.op    = PtrToUlong( &thunk->unix_thunk.addr );
    thunk->unix_thunk.addr  = PtrToUlong( unix_call_32to64 );
    thunk->unix_thunk.cs    = cs64_sel;

    status = NtProtectVirtualMemory( GetCurrentProcess(), (void **)&thunk, &size, PAGE_EXECUTE_READ, &old_prot );
    if (status) return status;
    wow64info->CpuFlags |= WOW64_CPUFLAGS_MSFT64;
    WINE_MESSAGE( "wow64native initialized: cs32=%04x ss32=%04x cs64=%04x avx=%u\n",
                  cs32_sel, ss32_sel, cs64_sel, pw_native_avx );
    return STATUS_SUCCESS;
}


NTSTATUS WINAPI BTCpuProcessInit(void)
{
    /* The pinned Wine caller ignores this status; Simulate enforces it. */
    process_status = process_init();
    return process_status;
}

static struct pw_native_thread_params current_thread_params(void)
{
    TEB *teb = NtCurrentTeb();
    struct pw_native_thread_params params = { PW_NATIVE_ABI_VERSION, 0 };
    if (teb->WowTebOffset)
        params.guest_teb = (ULONG_PTR)((char *)teb + teb->WowTebOffset);
    return params;
}

void WINAPI BTCpuThreadInit(void)
{
    struct pw_native_thread_params params = current_thread_params();
    WINE_UNIX_CALL(pw_native_thread_init, &params);
}

void WINAPI BTCpuThreadTerm(HANDLE thread, LONG status)
{
    if (!thread || thread == GetCurrentThread()) WINE_UNIX_CALL(pw_native_thread_term, NULL);
}

void * WINAPI pw_native_prepare_simulate(void)
{
    struct pw_native_thread_params params = current_thread_params();
    NTSTATUS status = process_status;
    if (!status) status = WINE_UNIX_CALL(pw_native_thread_get, &params);
    if (!status && !params.state) status = STATUS_INVALID_ADDRESS;
    if (status)
    {
        /* Do not dispatch a guest exception through uninitialized BOP code. */
        NtTerminateProcess(GetCurrentProcess(), status);
        RtlRaiseStatus(status);
    }
    return (void *)(ULONG_PTR)params.state;
}

/**********************************************************************
 *           BTCpuGetBopCode  (wow64cpu.@)
 */
void * WINAPI BTCpuGetBopCode(void)
{
    struct thunk_opcodes *thunk = (struct thunk_opcodes *)code_buffer;

    return &thunk->syscall_thunk;
}


/**********************************************************************
 *           __wine_get_unix_opcode  (wow64cpu.@)
 */
void * WINAPI __wine_get_unix_opcode(void)
{
    struct thunk_opcodes *thunk = (struct thunk_opcodes *)code_buffer;

    return &thunk->unix_thunk;
}


/**********************************************************************
 *           BTCpuIsProcessorFeaturePresent  (wow64cpu.@)
 */
BOOLEAN WINAPI BTCpuIsProcessorFeaturePresent( UINT feature )
{
    /* assume CPU features are the same for 32- and 64-bit */
    return RtlIsProcessorFeaturePresent( feature );
}


/**********************************************************************
 *           BTCpuGetContext  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuGetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return RtlWow64GetThreadContext( thread, ctx );
}


/**********************************************************************
 *           BTCpuSetContext  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuSetContext( HANDLE thread, HANDLE process, void *unknown, I386_CONTEXT *ctx )
{
    return RtlWow64SetThreadContext( thread, ctx );
}


/**********************************************************************
 *           BTCpuResetToConsistentState  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuResetToConsistentState( EXCEPTION_POINTERS *ptrs )
{
    CONTEXT *context = ptrs->ContextRecord;
    I386_CONTEXT wow_context;
    struct machine_frame
    {
        ULONG64 rip;
        ULONG64 cs;
        ULONG64 eflags;
        ULONG64 rsp;
        ULONG64 ss;
    } *machine_frame;

    if (context->SegCs == cs64_sel) return STATUS_SUCCESS;  /* exception in 64-bit code, nothing to do */

    copy_context_64to32( &wow_context, CONTEXT_I386_ALL, context );
    wow_context.EFlags &= ~(0x100|0x40000);
    BTCpuSetContext( GetCurrentThread(), GetCurrentProcess(), NULL, &wow_context );

    /* fixup context to pretend that we jumped to 64-bit mode */
    context->Rip = (ULONG64)syscall_32to64;
    context->SegCs = cs64_sel;
    context->Rsp = context->R14;
    context->SegSs = ds64_sel;
    /* fixup machine frame */
    machine_frame = (struct machine_frame *)(((ULONG_PTR)(ptrs->ExceptionRecord + 1) + 15) & ~15);
    machine_frame->rip = context->Rip;
    machine_frame->rsp = context->Rsp;
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           BTCpuTurboThunkControl  (wow64cpu.@)
 */
NTSTATUS WINAPI BTCpuTurboThunkControl( ULONG enable )
{
    if (enable) return STATUS_NOT_SUPPORTED;
    /* we don't have turbo thunks yet */
    return STATUS_SUCCESS;
}
