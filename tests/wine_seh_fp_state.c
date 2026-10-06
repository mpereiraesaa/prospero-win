/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A handled access violation keeps the thread's floating-point state: the
 * handler's CONTEXT has the MXCSR and XMM registers the faulting code had,
 * and what the handler writes into them is what execution resumes with
 * (patch 0501: the PS5 signal frame's FXSAVE image is at ucontext+320). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MXCSR_BEFORE 0x9fc0u /* FZ, DAZ, every exception masked */
#define MXCSR_HANDLER 0x7f80u /* every exception masked, round toward zero */

static const ULONGLONG xmm6_in[2] = { 0x0123456789abcdefull, 0xfedcba9876543210ull };
static const ULONGLONG xmm7_set[2] = { 0x5a5a5a5a5a5a5a5aull, 0xa5a5a5a5a5a5a5a5ull };
static volatile LONG handled;
static DWORD seen_mxcsr, seen_fltsave_mxcsr;
static ULONGLONG seen_xmm6[2];

extern char fault_store[], fault_resume[];

static LONG CALLBACK handler(EXCEPTION_POINTERS *info)
{
    CONTEXT *context = info->ContextRecord;

    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        context->Rip != (DWORD64)fault_store)
        return EXCEPTION_CONTINUE_SEARCH;
    handled++;
    seen_mxcsr = context->MxCsr;
    seen_fltsave_mxcsr = context->FltSave.MxCsr;
    memcpy(seen_xmm6, &context->Xmm6, sizeof(seen_xmm6));
    context->MxCsr = MXCSR_HANDLER;
    context->FltSave.MxCsr = MXCSR_HANDLER;
    memcpy(&context->Xmm7, xmm7_set, sizeof(xmm7_set));
    context->Rip = (DWORD64)fault_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(void)
{
    unsigned int mxcsr_in = MXCSR_BEFORE, mxcsr_out = 0, mxcsr_restore = 0x1f80u;
    ULONGLONG xmm6_out[2] = { 0 }, xmm7_out[2] = { 0 };
    void *null = NULL;
    int ok;

    AddVectoredExceptionHandler(1, handler);
    __asm__ volatile(
        "ldmxcsr %[in]\n\t"
        "movdqu %[x6],%%xmm6\n\t"
        "pxor %%xmm7,%%xmm7\n\t"
        ".globl fault_store\nfault_store:\n\t"
        "movl $0,(%[null])\n\t"
        ".globl fault_resume\nfault_resume:\n\t"
        "stmxcsr %[out]\n\t"
        "movdqu %%xmm6,%[x6out]\n\t"
        "movdqu %%xmm7,%[x7out]\n\t"
        "ldmxcsr %[restore]\n\t"
        : [out] "=m"(mxcsr_out), [x6out] "=m"(xmm6_out), [x7out] "=m"(xmm7_out)
        : [in] "m"(mxcsr_in), [x6] "m"(xmm6_in), [null] "r"(null), [restore] "m"(mxcsr_restore)
        : "memory", "xmm6", "xmm7");

    ok = handled == 1 && seen_mxcsr == MXCSR_BEFORE && seen_fltsave_mxcsr == MXCSR_BEFORE &&
         !memcmp(seen_xmm6, xmm6_in, sizeof(xmm6_in)) && mxcsr_out == MXCSR_HANDLER &&
         !memcmp(xmm6_out, xmm6_in, sizeof(xmm6_in)) && !memcmp(xmm7_out, xmm7_set, sizeof(xmm7_set));
    printf("seh-fp handled=%ld context_mxcsr=%#lx fltsave_mxcsr=%#lx context_xmm6=%016llx%016llx\n",
           handled, seen_mxcsr, seen_fltsave_mxcsr, seen_xmm6[1], seen_xmm6[0]);
    printf("seh-fp resumed mxcsr=%#x xmm6=%016llx%016llx xmm7=%016llx%016llx\n", mxcsr_out,
           xmm6_out[1], xmm6_out[0], xmm7_out[1], xmm7_out[0]);
    printf("seh-fp verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
