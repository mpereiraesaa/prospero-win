/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef WOW64NATIVE_H
#define WOW64NATIVE_H

#define PW_NATIVE_ABI_VERSION 4
#define PW_NATIVE_CS32 0x33
#define PW_NATIVE_SS32 0x3b
#define PW_NATIVE_CS64 0x43

/* ntdll owns the signal protocol. Its optional Unix export accepts this
 * version and returns zero for an unsupported protocol. Each bit describes
 * an installed protocol, not a request to enable an unimplemented path. */
#define PW_NW64_CAPS_VERSION 1u
#define PW_NATIVE_SIGNAL_QUERY "__wine_prospero_native_wow64_caps"
#define PW_NW64_CAP_RAW_TITLESAFE_HANDLERS 0x01u
#define PW_NW64_CAP_HOST_FS_BEFORE_C 0x02u
#define PW_NW64_CAP_CORRECTED_FP_XSTATE_CTX 0x04u
#define PW_NW64_CAP_REQUIRED (PW_NW64_CAP_RAW_TITLESAFE_HANDLERS | \
                              PW_NW64_CAP_HOST_FS_BEFORE_C | \
                              PW_NW64_CAP_CORRECTED_FP_XSTATE_CTX)
/* Bits accumulate within version 1. Reserved optional proofs: 0x08 for
 * compatibility-mode signal return and 0x10 for repeated delivery. */
unsigned __wine_prospero_native_wow64_caps(unsigned version);

/* Shared initialization contract. Ready is set only after the complete
 * per-thread FS transition and fault protocol is available. */
struct pw_native_init_params
{
    unsigned int version;
    unsigned int transitions_ready;
    unsigned int fs32_selector;
    unsigned int reserved;
    unsigned long long fs_set_proc;
};
/* Version 3 adds an optional per-thread diagnostic. Counters are written
 * only by transition assembly on this OS thread. Snapshot callback runs
 * only after host FS is restored, inside the existing full FP save.
 * Version 4 adds the time spent on the host side between a guest-to-host
 * and the next host-to-guest switch, by what entered the host (host_kind,
 * set by the Unix-call and syscall entries and cleared on the way back),
 * and a TSC deadline for the next report. */
struct pw_native_profile
{
    unsigned long long host_calls, guest_calls;
    unsigned long long host_sysarch_ticks, guest_sysarch_ticks;
    unsigned long long unix_calls, syscall_calls, last_tsc;
    unsigned long long host_kind, report_proc;
    unsigned long long unix_host_ticks, syscall_host_ticks, other_host_ticks;
    unsigned long long next_report_tsc;
};
#define PW_NATIVE_HOST_UNIX 1
#define PW_NATIVE_HOST_SYSCALL 2
/* Unix-owned thread state; accessing Unix TLS requires host FS. */
struct pw_native_thread_state
{
    unsigned long long host_fs;
    unsigned long long guest_fs;
    unsigned int status;
    unsigned int tid;           /* Windows thread id, for the profile report */
    unsigned long long profile;
};
struct pw_native_thread_params
{
    unsigned int version;
    unsigned int tid;           /* Windows thread id (version 4) */
    unsigned long long guest_teb;
    unsigned long long state;
};
enum pw_native_funcs
{
    pw_native_process_init,
    pw_native_thread_init,
    pw_native_thread_get,
    pw_native_thread_term,
    pw_native_funcs_count
};
#endif
