/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Initial platform contract for the hardware WoW64 backend. */
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "wow64native.h"
#include "wine/debug.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

WINE_DEFAULT_DEBUG_CHANNEL(wow);
#ifdef __PROSPERO__
#include <dlfcn.h>
#endif

static __thread struct pw_native_thread_state thread_state;
static __thread int thread_initialized;
static int process_ready;
static int profile_enabled;
static unsigned long long profile_tsc_hz;
static __thread struct pw_native_profile thread_profile;

static unsigned long long read_tsc(void)
{
    unsigned int low, high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high));
    return ((unsigned long long)high << 32) | low;
}

/* The TSC rate the reports are paced by and printed with: PW_QPC_TSC_HZ when
 * the title measured it, else a 20 ms measurement against CLOCK_MONOTONIC.
 * 0 when neither works; reports then come every 262144 host switches only. */
static unsigned long long measure_tsc_hz(void)
{
    const char *text = getenv("PW_QPC_TSC_HZ");
    struct timespec start, now;
    unsigned long long tsc0, tsc1, ns;
    if (text && *text)
    {
        char *end;
        unsigned long long hz = strtoull(text, &end, 10);
        if (!*end && hz >= 100000000ull && hz <= 10000000000ull) return hz;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return 0;
    tsc0 = read_tsc();
    do
    {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
        ns = (unsigned long long)(now.tv_sec - start.tv_sec) * 1000000000ull + now.tv_nsec - start.tv_nsec;
    } while (ns < 20000000ull);
    tsc1 = read_tsc();
    return tsc1 > tsc0 ? (tsc1 - tsc0) * 1000000000ull / ns : 0;
}

/* The core clock this thread runs at: a chain of dependent 1-cycle adds
 * (eight per iteration, so the loop counter runs alongside) timed with
 * CLOCK_MONOTONIC. 2M iterations are 16M cycles: under 5 ms at 3.5 GHz,
 * 10 ms at 1.6 GHz. The TSC runs at a fixed rate whatever the core does,
 * so only this shows a lower power state. cpu is RDTSCP's TSC_AUX (the
 * CPU number where the kernel sets it). */
static void report_cpu_clock(unsigned int tid)
{
    struct timespec start, end;
    unsigned long long iterations = 2000000ull, chain = 0, ns;
    unsigned int low, high, aux;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return;
    __asm__ volatile("1:\n\t"
                     "addq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\t"
                     "addq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\taddq $1,%0\n\t"
                     "decq %1\n\tjnz 1b"
                     : "+r"(chain), "+r"(iterations) : : "cc");
    if (clock_gettime(CLOCK_MONOTONIC, &end)) return;
    __asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(aux));
    ns = (unsigned long long)(end.tv_sec - start.tv_sec) * 1000000000ull + end.tv_nsec - start.tv_nsec;
    if (!ns || chain != 16000000ull) return;
    WINE_MESSAGE("PW_NATIVE_PROFILE cpu_clock_mhz=%llu cpu=%u tid=%04x\n", chain * 1000ull / ns, aux, tid);
}
static unsigned long long clock_next_tsc;

/* Cumulative per-thread counters; tools/native_profile_split.py turns two
 * consecutive reports of a thread into an interval's split. tid is the
 * Windows thread id, as in Wine's log prefixes. */
static void profile_report(void *arg)
{
    const struct pw_native_thread_state *state = arg;
    struct pw_native_profile *p = (void *)state->profile;
    if (!p) return;
    WINE_MESSAGE("PW_NATIVE_PROFILE version=2 tid=%04x teb=%llx tsc=%llu tsc_hz=%llu host_calls=%llu guest_calls=%llu "
                 "host_sysarch_ticks=%llu guest_sysarch_ticks=%llu unix_calls=%llu syscall_calls=%llu "
                 "unix_host_ticks=%llu syscall_host_ticks=%llu other_host_ticks=%llu\n",
                 state->tid, state->guest_fs, p->last_tsc, profile_tsc_hz, p->host_calls, p->guest_calls,
                 p->host_sysarch_ticks, p->guest_sysarch_ticks, p->unix_calls, p->syscall_calls,
                 p->unix_host_ticks, p->syscall_host_ticks, p->other_host_ticks);
    p->next_report_tsc = profile_tsc_hz ? p->last_tsc + 2 * profile_tsc_hz : ~0ull;
    /* About once a minute, on whichever thread reports first after that:
     * the busy ones report most often. */
    if (profile_tsc_hz && p->last_tsc >= __atomic_load_n(&clock_next_tsc, __ATOMIC_RELAXED))
    {
        __atomic_store_n(&clock_next_tsc, p->last_tsc + 60 * profile_tsc_hz, __ATOMIC_RELAXED);
        report_cpu_clock(state->tid);
    }
}

static NTSTATUS capture_host_fs(unsigned long long *base)
{
    *base = 0;
#ifdef __PROSPERO__
    extern int sysarch(int, void *);
    /* Called with host FS active, never from a guest-FS transition. */
    if (sysarch(128 /* AMD64_GET_FSBASE */, base)) return STATUS_UNSUCCESSFUL;
    if (!*base) return STATUS_INVALID_ADDRESS;
    return STATUS_SUCCESS;
#elif defined(__linux__) && defined(__x86_64__)
    *base = (unsigned long long)__builtin_thread_pointer();
    return STATUS_SUCCESS;
#else
    return STATUS_NOT_SUPPORTED;
#endif
}

static NTSTATUS process_init(void *args)
{
    struct pw_native_init_params *params = args;
    process_ready = 0;
    profile_enabled = 0;
    if (!params || params->version != PW_NATIVE_ABI_VERSION)
        return STATUS_INVALID_PARAMETER;
    params->transitions_ready = 0;
    params->fs32_selector = 0;
    params->fs_set_proc = 0;
#ifdef __PROSPERO__
    {
        extern int sysarch(int, void *);
        unsigned int (*query)(unsigned int);
        unsigned int caps;
        /* Resolve through ntdll's Wine dl facade: this optional export is
         * absent in older runtimes, which must keep the backend disabled. */
        query = dlsym(RTLD_DEFAULT, PW_NATIVE_SIGNAL_QUERY);
        if (!query) return STATUS_NOT_SUPPORTED;
        caps = query(PW_NW64_CAPS_VERSION);
        if ((caps & PW_NW64_CAP_REQUIRED) != PW_NW64_CAP_REQUIRED)
            return STATUS_NOT_SUPPORTED;
        params->fs_set_proc = (unsigned long long)sysarch;
        params->transitions_ready = 1;
        process_ready = 1;
        {
            const char *value = getenv("PW_NATIVE_PROFILE");
            profile_enabled = value && !strcmp(value, "1");
            if (profile_enabled)
            {
                profile_tsc_hz = measure_tsc_hz();
                report_cpu_clock(0);    /* at startup, before any thread report */
                clock_next_tsc = read_tsc() + 60 * profile_tsc_hz;
            }
        }
        return STATUS_SUCCESS;
    }
#endif
    /* Non-console builds never authorize native console execution. */
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS thread_init(void *args)
{
    struct pw_native_thread_params *params = args;
    NTSTATUS status;
    if (!params || params->version != PW_NATIVE_ABI_VERSION) return STATUS_INVALID_PARAMETER;
    params->state = 0;
    if (params->guest_teb < 0x10000 || params->guest_teb > 0xfffff000)
        return STATUS_INVALID_ADDRESS;
    if (thread_initialized)
    {
        if (thread_state.guest_fs != params->guest_teb) return STATUS_INVALID_PARAMETER;
        thread_state.status = process_ready ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
        params->state = (unsigned long long)&thread_state;
        return thread_state.status;
    }
    status = capture_host_fs(&thread_state.host_fs);
    if (status) return status;
    thread_profile = (struct pw_native_profile){0};
    if (profile_enabled)
    {
        thread_profile.report_proc = (unsigned long long)profile_report;
        thread_state.profile = (unsigned long long)&thread_profile;
    }
    thread_state.guest_fs = params->guest_teb;
    thread_state.tid = params->tid;
    thread_state.status = process_ready ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    thread_initialized = 1;
    params->state = (unsigned long long)&thread_state;
    return thread_state.status;
}

static NTSTATUS thread_get(void *args)
{
    struct pw_native_thread_params *params = args;
    if (!params || params->version != PW_NATIVE_ABI_VERSION) return STATUS_INVALID_PARAMETER;
    params->state = 0;
    if (!thread_initialized || thread_state.guest_fs != params->guest_teb)
        return STATUS_INVALID_PARAMETER;
    thread_state.status = process_ready ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    params->state = (unsigned long long)&thread_state;
    return thread_state.status;
}

static NTSTATUS thread_term(void *args)
{
    profile_report(&thread_state);
    thread_profile = (struct pw_native_profile){0};
    thread_state = (struct pw_native_thread_state){0};
    thread_initialized = 0;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = { process_init, thread_init, thread_get, thread_term };
C_ASSERT(sizeof(struct pw_native_thread_state) == 32);
C_ASSERT(sizeof(struct pw_native_init_params) == 24);
C_ASSERT(sizeof(struct pw_native_thread_params) == 24);
C_ASSERT(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == pw_native_funcs_count);
