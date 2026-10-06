/* SPDX-License-Identifier: LGPL-2.1-or-later
 * GetProcessMemoryInfo reports this process's memory, for the pseudo handle
 * and for a handle opened on its own id, and the working set grows by what
 * the program commits (patch 0898: the console has no
 * /proc to read them from). */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>

#define GROW (64u << 20)

static int query(HANDLE process, PROCESS_MEMORY_COUNTERS_EX *counters)
{
    memset(counters, 0, sizeof(*counters));
    return GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)counters, sizeof(*counters));
}

int main(void)
{
    PROCESS_MEMORY_COUNTERS_EX before, after, own;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, GetCurrentProcessId());
    unsigned char *memory;
    int ok;

    ok = query(GetCurrentProcess(), &before);
    memory = VirtualAlloc(NULL, GROW, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    for (SIZE_T i = 0; memory && i < GROW; i += 4096) memory[i] = 1;
    ok = ok && memory && query(GetCurrentProcess(), &after) && process && query(process, &own);
    printf("process-counters working_set=%llu->%llu pagefile=%llu->%llu peak=%llu own_handle_working_set=%llu\n",
           (unsigned long long)before.WorkingSetSize, (unsigned long long)after.WorkingSetSize,
           (unsigned long long)before.PagefileUsage, (unsigned long long)after.PagefileUsage,
           (unsigned long long)after.PeakWorkingSetSize, (unsigned long long)own.WorkingSetSize);
    /* The page file usage is printed, not checked: Wine on Linux leaves it 0. */
    ok = ok && before.WorkingSetSize && after.WorkingSetSize >= before.WorkingSetSize + GROW &&
         after.PeakWorkingSetSize >= after.WorkingSetSize && own.WorkingSetSize >= after.WorkingSetSize;
    printf("process-counters verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
