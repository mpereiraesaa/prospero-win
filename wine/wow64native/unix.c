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

WINE_DEFAULT_DEBUG_CHANNEL(wow);
#ifdef __PROSPERO__
#include <dlfcn.h>
#endif

static __thread struct pw_native_thread_state thread_state;
static __thread int thread_initialized;
static int process_ready;
static int profile_enabled;
static __thread struct pw_native_profile thread_profile;
static void profile_report(void *arg)
{
    const struct pw_native_thread_state *state = arg;
    const struct pw_native_profile *p = (void *)state->profile;
    if (!p) return;
    WINE_MESSAGE("PW_NATIVE_PROFILE version=1 teb=%llx tsc=%llu host_calls=%llu guest_calls=%llu host_sysarch_ticks=%llu guest_sysarch_ticks=%llu unix_calls=%llu syscall_calls=%llu\n",
                 state->guest_fs, p->last_tsc, p->host_calls, p->guest_calls,
                 p->host_sysarch_ticks, p->guest_sysarch_ticks, p->unix_calls, p->syscall_calls);
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
