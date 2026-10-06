/* SPDX-License-Identifier: LGPL-2.1-or-later
 * The processor times Windows reports keep their meaning: each CPU's kernel
 * time includes its idle time, and user time grows while the program runs
 * (patch 0889: the console refuses kern.cp_times). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>

typedef struct
{
    LARGE_INTEGER IdleTime, KernelTime, UserTime, Reserved1[2];
    ULONG Reserved2;
} PERFORMANCE;

static ULONGLONG user_total(const PERFORMANCE *cpus, unsigned count)
{
    ULONGLONG total = 0;
    for (unsigned i = 0; i < count; i++) total += cpus[i].UserTime.QuadPart;
    return total;
}

int main(void)
{
    static PERFORMANCE before[64], after[64];
    ULONG size = 0;
    unsigned count, kernel_ok = 1;
    volatile ULONGLONG spin = 0;
    DWORD start;
    NTSTATUS status;

    status = NtQuerySystemInformation(SystemProcessorPerformanceInformation, before, sizeof(before), &size);
    count = size / sizeof(PERFORMANCE);
    start = GetTickCount();
    while (GetTickCount() - start < 300) spin++;
    NtQuerySystemInformation(SystemProcessorPerformanceInformation, after, sizeof(after), &size);
    for (unsigned i = 0; i < count; i++)
        kernel_ok &= after[i].KernelTime.QuadPart >= after[i].IdleTime.QuadPart && after[i].IdleTime.QuadPart > 0;
    printf("processor-times status=%#lx cpus=%u cpu0 idle=%lld kernel=%lld user=%lld user_growth=%llu\n",
           status, count, after[0].IdleTime.QuadPart, after[0].KernelTime.QuadPart, after[0].UserTime.QuadPart,
           user_total(after, count) - user_total(before, count));
    kernel_ok = !status && count && kernel_ok && user_total(after, count) > user_total(before, count);
    printf("processor-times verdict=%s\n", kernel_ok ? "pass" : "fail");
    fflush(stdout);
    return kernel_ok ? 0 : 1;
}
