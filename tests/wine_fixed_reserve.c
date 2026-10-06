/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Memory reserved at an address of the program's choosing, outside the
 * ranges Wine reserves up front, can be committed and written, also after
 * it was released and reserved again (patch 0897: on the PS5 a bare
 * reservation is not backed when it is protected). */
#include <windows.h>
#include <stdio.h>

#define RESERVE 0x10000
#define COMMIT 0x1000

static volatile LONG faults;

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info)
{
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    faults++;
    info->ContextRecord->Rip += 3; /* movb $imm8,(%rax) without displacement: c6 00 xx */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void poke(volatile unsigned char *where)
{
    __asm__ volatile("movb $0x5a,(%0)" :: "a"(where) : "memory");
}

/* 1 written and read back, 0 the write faulted, -1 not reservable there. */
static int try_at(ULONG_PTR address, int round)
{
    unsigned char *base = VirtualAlloc((void *)address, RESERVE, MEM_RESERVE, PAGE_NOACCESS);
    unsigned char *page;
    LONG before = faults;
    int ok;

    if (base != (unsigned char *)address)
    {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
        return -1;
    }
    page = VirtualAlloc(base + COMMIT, COMMIT, MEM_COMMIT, PAGE_READWRITE);
    ok = page == base + COMMIT;
    if (ok)
    {
        poke(page);
        ok = faults == before && page[0] == 0x5a && page[1] == 0;
    }
    printf("fixed-reserve %#llx round %d commit=%p faults=%ld %s\n", (unsigned long long)address, round, page,
           faults - before, ok ? "ok" : "wrong");
    VirtualFree(base, 0, MEM_RELEASE);
    return ok;
}

int main(void)
{
    static const ULONG_PTR addresses[] = { 0x200000000ull, 0x3000000000ull, 0x7ff000000000ull };
    int tried = 0, failed = 0;

    AddVectoredExceptionHandler(1, on_fault);
    for (unsigned i = 0; i < ARRAYSIZE(addresses); i++)
        for (int round = 0; round < 2; round++)
        {
            int result = try_at(addresses[i], round);
            if (result < 0) continue;
            tried++;
            failed += !result;
        }
    printf("fixed-reserve verdict=%s tried=%d\n", tried && !failed ? "pass" : "fail", tried);
    fflush(stdout);
    return tried && !failed ? 0 : 1;
}
