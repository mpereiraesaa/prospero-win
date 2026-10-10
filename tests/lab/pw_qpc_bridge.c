/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "pw_qpc_clock.h"
#define ReadAcquire(p) (*(volatile LONG *)(p))
#ifndef DECLSPEC_ALIGN
#define DECLSPEC_ALIGN(n) __attribute__((aligned(n)))
#endif

typedef int PROCESSINFOCLASS;
static uint64_t mock_tsc = UINT64_C(500000000000), mock_counter = 1000;
static LONG anchors, slows;
static unsigned tsc_reads;
static int available = 1;

static LONG mock_query(HANDLE process, PROCESSINFOCLASS cls, void *args,
                       ULONG bytes, void *retlen)
{
    (void)retlen;
    assert(process == GetCurrentProcess());
    assert((unsigned)cls == PW_QPC_PROCESS_INFO && bytes == 48);
    InterlockedIncrement(&anchors);
    if (!available) return -1;
    assert(pw_qpc_make_anchor(args, mock_tsc, mock_counter, UINT64_C(3200000000), 1));
    if (available == 2) ((struct pw_qpc_anchor *)args)->version++;
    return 0;
}

static LONG mock_slow(LARGE_INTEGER *counter, void *frequency)
{
    (void)frequency;
    InterlockedIncrement(&slows);
    counter->QuadPart = mock_counter;
    return 0;
}

static void mock_read_tsc(unsigned *lo, unsigned *hi)
{
    ++tsc_reads;
    *lo = (uint32_t)mock_tsc;
    *hi = (uint32_t)(mock_tsc >> 32);
}

#define NtCurrentProcess() GetCurrentProcess()
#define NtQueryInformationProcess mock_query
#define NtQueryPerformanceCounter mock_slow
#include "qpc_body.inc"

static DWORD WINAPI worker(void *unused)
{
    LARGE_INTEGER c;
    unsigned i;
    (void)unused;
    for (i = 0; i < 10000; ++i)
    {
        fixture_qpc(&c);
        assert(c.QuadPart >= 15001000);
    }
    return 0;
}

int main(int argc, char **argv)
{
    LARGE_INTEGER c;
    assert(argc == 2);
    if (!strcmp(argv[1], "unsupported") || !strcmp(argv[1], "malformed"))
    {
        available = !strcmp(argv[1], "malformed") ? 2 : 0;
        fixture_qpc(&c);
        assert(c.QuadPart == 1000);
        ++mock_counter;
        fixture_qpc(&c);
        assert(c.QuadPart == 1001 && anchors == 1 && slows == 2 && !tsc_reads);
    }
    else
    {
        HANDLE threads[8];
        unsigned i;
        fixture_qpc(&c);
        assert(c.QuadPart == 1000 && anchors == 1 && !slows);
        mock_tsc += UINT64_C(1600000000);
        fixture_qpc(&c);
        assert(c.QuadPart >= 5000999 && c.QuadPart <= 5001000 && anchors == 1 && !slows);
        mock_tsc += UINT64_C(3200000000);
        mock_counter = 15001000;
        fixture_qpc(&c);
        assert(c.QuadPart == 15001000 && anchors == 2 && !slows);
        available = 0;
        mock_tsc += UINT64_C(4000000000);
        mock_counter = 1000;
        fixture_qpc(&c);
        assert(c.QuadPart == 15001000 && anchors == 3 && slows == 1);
        fixture_qpc(&c);
        assert(c.QuadPart == 15001000 && anchors == 3 && slows == 2);
        for (i = 0; i < 8; ++i) threads[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        assert(WaitForMultipleObjects(8, threads, TRUE, 20000) == WAIT_OBJECT_0);
        for (i = 0; i < 8; ++i) CloseHandle(threads[i]);
    }
    puts("PASS PE QPC body fallback/expiry/monotonic/concurrency; simulated TSC provider");
    return 0;
}
