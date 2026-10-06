/* SPDX-License-Identifier: LGPL-2.1-or-later
 * How a game's busy threads are scheduled: how many CPUs Windows is told
 * about against the ones threads run on (RDTSCP's processor id), and whether
 * N busy threads all start at once or queue behind each other. Patch 0881
 * reports the 13 CPUs a PS5 game runs on; patch 0882 lets threads of one
 * priority take turns. Every busy thread stops itself after SPIN_MS by its
 * own clock, so a scheduler that never shares a CPU cannot keep the title's
 * other threads off the CPUs for long. */
#include <windows.h>
#include <stdio.h>
#include <x86intrin.h>

#define SPIN_MS 200
#define MAX_THREADS 24

static LARGE_INTEGER frequency, created;
static volatile LONG go;
static LONGLONG start_delay_us[MAX_THREADS];
static unsigned long long cpus_seen[MAX_THREADS];

static LONGLONG now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static DWORD WINAPI spin(void *arg)
{
    int index = (int)(INT_PTR)arg;
    LONGLONG first = now(), deadline = first + frequency.QuadPart * SPIN_MS / 1000;
    unsigned int aux;

    start_delay_us[index] = (first - created.QuadPart) * 1000000 / frequency.QuadPart;
    while (now() < deadline)
    {
        __rdtscp(&aux);
        cpus_seen[index] |= 1ull << ((aux & 0xfff) & 63);
    }
    return 0;
}

static int run(int count, int raise_last)
{
    HANDLE threads[MAX_THREADS];
    LONGLONG worst = 0, total_delay = 0;
    unsigned long long cpus = 0;
    int late = 0;

    memset(start_delay_us, 0, sizeof(start_delay_us));
    memset(cpus_seen, 0, sizeof(cpus_seen));
    created.QuadPart = now();
    for (int i = 0; i < count; i++)
    {
        threads[i] = CreateThread(NULL, 0, spin, (void *)(INT_PTR)i, CREATE_SUSPENDED, NULL);
        if (raise_last && i == count - 1) SetThreadPriority(threads[i], THREAD_PRIORITY_TIME_CRITICAL);
    }
    created.QuadPart = now();
    for (int i = 0; i < count; i++) ResumeThread(threads[i]);
    WaitForMultipleObjects(count, threads, TRUE, 10000);
    for (int i = 0; i < count; i++)
    {
        CloseHandle(threads[i]);
        if (start_delay_us[i] > worst) worst = start_delay_us[i];
        total_delay += start_delay_us[i];
        if (start_delay_us[i] > SPIN_MS * 1000 / 2) late++;
        cpus |= cpus_seen[i];
    }
    printf("sched-probe threads=%d%s late=%d worst_start_ms=%lld mean_start_ms=%lld last_start_ms=%lld cpus_used=%d mask=%#llx\n",
           count, raise_last ? " (last TIME_CRITICAL)" : "", late, worst / 1000, total_delay / count / 1000,
           start_delay_us[count - 1] / 1000, __builtin_popcountll(cpus), cpus);
    fflush(stdout);
    Sleep(300);
    return late;
}

int main(void)
{
    static const int counts[] = { 4, 8, 12, 13, 14, 16, 20 };
    SYSTEM_INFO info;
    DWORD_PTR process_mask = 0, system_mask = 0;
    int late;

    QueryPerformanceFrequency(&frequency);
    GetSystemInfo(&info);
    GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
    printf("sched-probe windows_cpus=%lu process_mask=%#llx system_mask=%#llx\n", info.dwNumberOfProcessors,
           (unsigned long long)process_mask, (unsigned long long)system_mask);
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) run(counts[i], 0);
    run(16, 1);
    /* As many busy threads as Windows reports CPUs must all run at once. */
    late = run(info.dwNumberOfProcessors < MAX_THREADS ? (int)info.dwNumberOfProcessors : MAX_THREADS, 0);
    printf("sched-probe verdict=%s\n", late ? "fail" : "pass");

    fflush(stdout);
    return 0;
}
