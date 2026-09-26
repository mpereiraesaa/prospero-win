/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * prospero-win's non-executing PE mapping validator for the native console.
 *
 * The title stages a PE image and its third-party dependencies under a
 * read-only directory, manually maps every one of them, verifies the
 * mapping byte by byte and reports the whole graph through ps5log/1.
 * The staged PE is never executed. A separate synthetic Win64 call probe
 * checks the explicit integer ABI bridge before mapping. Neither
 * result establishes execution of the original Windows application.
 *
 * Teardown follows the laboratory's measured rule: after the report and the
 * BYE the runtime calls _exit(0) instead of returning from main(), because
 * the CRT exit path leaves the shell showing a failure dialog on FW 12.02.
 */
#include "../src/pw_gate.h"
#include "../src/pw_vm_posix.h"
#include "../src/pw_exec_probe.h"
#include "pw_compat32_ps5.h"
#include "pw_lowmem_ps5.h"
#include "pw_wine_platform_ps5.h"
#include "pw_ucontext_map_ps5.h"
#include "pw_vmspace_ps5.h"
#include "pw_file_ps5.h"
#include "../wine/ps5/pw_wine_unix_probe.h"
#include "ps5log/ps5log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#define PW_TITLE_ID "PPSA99995"
#define PW_APP_NAME "prospero-win"

#ifndef PW_STAGE_DIR
#define PW_STAGE_DIR "/app0/win"
#endif
#ifndef PW_ROOT_MODULE
#define PW_ROOT_MODULE "sample.exe"
#endif
/*
 * The compatibility-mode diagnostic first installs the descriptors and
 * reports without transferring control. Its optional second step performs
 * the far transfer and may fault, so the first record is flushed beforehand.
 */
#ifndef PW_COMPAT32_TRANSFER
#define PW_COMPAT32_TRANSFER 0
#endif

static PwFilePs5 files;
static PwFileProvider provider;
static PwVmBackend backend;
static PwCompat32Ps5 compat32_state;
static PwCompat32Platform compat32_platform;

/* Declared so the fault reporter can express the program counter as an
 * offset from a known symbol. */
int main(int argc, char **argv);

extern int32_t sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t,
                                        const void *, int *);
extern int sceKernelGetModuleInfo(int32_t, void *);

static void log_wine_unix_step(int step)
{
    PS5LOG_LOG("PW_WINE_UNIX step=%s", pw_wine_unix_probe_step_name(step));
}

/*
 * Without this, a fault truncates the transcript and says nothing: the
 * first run of this title stopped after four records with no indication of
 * where or why. The porting playbook's fault table reads `pc` outside the
 * image with `rax == pc` and a null address as a call through a broken
 * imported symbol, so the program counter, the fault address and rax are
 * all reported, with pc given relative to main() so it can be symbolised
 * offline against build/native/eboot.elf.
 */
static void fatal_signal(int number, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;
    const uintptr_t base = (uintptr_t)&main;
    const uintptr_t pc = uc ? (uintptr_t)uc->uc_mcontext.mc_rip : 0u;

    PS5LOG_LOG("PW_SIGNAL sig=%d code=%d addr=%p pc=%p main=%p "
               "pc_minus_main=%ld rax=%p rsp=%p rdi=%p",
               number, info ? info->si_code : 0,
               info ? info->si_addr : NULL, (void *)pc, (void *)base,
               (long)(pc - base),
               uc ? (void *)uc->uc_mcontext.mc_rax : NULL,
               uc ? (void *)uc->uc_mcontext.mc_rsp : NULL,
               uc ? (void *)uc->uc_mcontext.mc_rdi : NULL);
    ps5log_close("pe-map-crashed");
    _exit(1);
}

static void install_signal_reporter(void)
{
    static const int signals[] = {
        SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS,
    };
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_sigaction = fatal_signal;
    act.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]);
         ++index)
        (void)sigaction(signals[index], &act, NULL);
}

/* Streams one gate record as soon as it exists, so a fault cannot take the
 * evidence with it. */
static void emit_line(const char *line, void *context)
{
    (void)context;
    PS5LOG_LOG("%s", line);
}

static uint64_t now_ns(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0u;
    return (uint64_t)value.tv_sec * 1000000000ull + (uint64_t)value.tv_nsec;
}

/*
 * The loader registry and the gate report are far too large for the libc
 * heap, which is about 8 MiB here and cannot be grown from a title. Both
 * come from an anonymous mapping, the measured route for large allocations.
 */
static void *reserve_scratch(size_t bytes)
{
    void *base = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return base == MAP_FAILED ? NULL : base;
}

int main(int argc, char **argv)
{
    const uint64_t boot_token = now_ns();
    const char *stage_dir = PW_STAGE_DIR;
    const char *root_name = PW_ROOT_MODULE;
    ps5log_config log_config;
    const char *log_path = NULL;
    PwFilePs5Smoke smoke;
    PwGateRequest request;
    PwGateReport *report;
    PwLoader *loader;
    PwFileSpan root;
    int config_result;
    int log_result;
    int status;

    (void)argc;
    (void)argv;

    ps5log_config_defaults(&log_config);
    config_result = ps5log_load_config(ps5log_default_conf_paths,
                                        ps5log_default_conf_path_count,
                                        &log_config, &log_path);
    log_result = config_result == 0
        ? ps5log_init(&log_config, PW_TITLE_ID, PW_APP_NAME, boot_token)
        : config_result;

    install_signal_reporter();

    loader = reserve_scratch(sizeof(*loader));
    report = reserve_scratch(sizeof(*report));
    if (!loader || !report) {
        PS5LOG_LOG("PW_ABORT stage=scratch loader=%d report=%d",
                   loader != NULL, report != NULL);
        ps5log_close("pe-map-scratch-unavailable");
        _exit(0);
    }

    PS5LOG_LOG("PW_BEGIN schema=1 slice=pe-map title=%s stage=%s root=%s "
               "log_config=%d log_init=%d loader_bytes=%llu",
               PW_TITLE_ID, stage_dir, root_name, config_result, log_result,
               (unsigned long long)sizeof(*loader));

    {
        /*
         * Before the PE staging checks, which it does not need: Wine's
         * Unix side as PRX modules, when the build packaged them
         * (PW_WINE_PS5_PRX_DIR): ntdll.prx through the firmware loader, its
         * descriptor, module_start, adopt and heap, then win32u through
         * ntdll's own dlopen. Each step is logged before it runs.
         */
        /* access() reported the uploaded PRX absent on FW 12.02, so the
         * check opens it, as the file provider does, and logs errno. */
        static const char dir[] = "/app0/win/wine";
        int prx_fd = open("/app0/win/wine/ntdll.prx", O_RDONLY);
        int prx_errno = prx_fd < 0 ? errno : 0;
        if (prx_fd >= 0)
            close(prx_fd);
        if (prx_fd >= 0) {
            const PwWineUnixProbeOps ops = {
                sceKernelLoadStartModule, sceKernelGetModuleInfo, log_wine_unix_step };
            PwWineUnixProbeReport unix_report;
            int rc = pw_wine_unix_probe(&ops, dir, &unix_report);

            PS5LOG_LOG("PW_WINE_UNIX rc=%d step=%s handle=0x%x start_result=%d "
                       "module_start=%d missing=0x%x error=%s",
                       rc, pw_wine_unix_probe_step_name(unix_report.step),
                       (unsigned)unix_report.ntdll_handle, unix_report.start_result,
                       unix_report.module_start_rc, unix_report.missing_exports,
                       unix_report.error[0] ? unix_report.error : "-");
            PS5LOG_LOG("PW_WINE_UNIX heap allocations=%llu->%llu frees=%llu->%llu "
                       "live=%llu mapped=%llu failures=%llu foreign=%llu",
                       (unsigned long long)unix_report.heap_before.allocations,
                       (unsigned long long)unix_report.heap_after.allocations,
                       (unsigned long long)unix_report.heap_before.frees,
                       (unsigned long long)unix_report.heap_after.frees,
                       (unsigned long long)unix_report.heap_after.live_bytes,
                       (unsigned long long)unix_report.heap_after.mapped_bytes,
                       (unsigned long long)unix_report.heap_after.failures,
                       (unsigned long long)unix_report.heap_after.foreign_frees);
        } else {
            PS5LOG_LOG("PW_WINE_UNIX absent dir=%s errno=%d", dir, prx_errno);
        }
    }

    status = pw_file_ps5_init(&files, stage_dir);
    if (status != PW_OK) {
        PS5LOG_LOG("PW_ABORT stage=provider status=%s",
                   pw_result_name(status));
        ps5log_close("pe-map-provider-rejected");
        _exit(0);
    }

    /*
     * Platform pre-flight before any parsing: prove the exact calls the
     * provider depends on, on this firmware, against the staged bytes.
     */
    status = pw_file_ps5_smoke(&files, root_name, &smoke);
    PS5LOG_LOG("PW_FS_SMOKE status=%s open=%d stat=%d read=%d seek=%d "
               "close=%d size=%lld magic=0x%02x%02x is_pe=%d",
               pw_result_name(status), smoke.open_result, smoke.stat_result,
               smoke.read_result, smoke.seek_result, smoke.close_result,
               smoke.size, smoke.first_bytes[0], smoke.first_bytes[1],
               smoke.is_pe);
    if (status != PW_OK) {
        ps5log_close("pe-map-filesystem-unusable");
        _exit(0);
    }

    /*
     * Once telemetry and the filesystem are known good, record whether the
     * title can reach 32-bit compatibility mode. The measured refusal is why
     * PE32 applications use the DBT; this diagnostic remains a bounded
     * regression check.
     */
    {
        /*
         * Low-address availability. A 32-bit guest needs its address space
         * below 4 GiB for a register used as an address to be correct
         * without translation, so this bounds the JIT recompilation route
         * that the compatibility-mode refusal leaves as the alternative.
         */
        PwLowMemReport lowmem;

        if (pw_lowmem_ps5_probe(&lowmem) == PW_OK) {
            for (uint32_t index = 0; index < lowmem.attempts && index < 8u;
                 ++index)
                PS5LOG_LOG("PW_LOWMEM_TRY bytes=%llu addr=0x%llx "
                           "honoured=%u below4g=%u",
                           (unsigned long long)lowmem.results[index].requested_bytes,
                           (unsigned long long)lowmem.results[index].address,
                           lowmem.results[index].honoured_hint,
                           lowmem.results[index].below_four_gib);
            PS5LOG_LOG("PW_LOWMEM largest_low_bytes=%llu base=0x%llx "
                       "write_read=%d mprotect_rx=%d mprotect_rwx=%d",
                       (unsigned long long)lowmem.largest_low_bytes,
                       (unsigned long long)lowmem.largest_low_base,
                       lowmem.write_read_ok, lowmem.mprotect_rx,
                       lowmem.mprotect_rwx);
        }
    }

    {
        /*
         * Facts Wine's native side needs before its x86_64 PE modules, Unix
         * libraries and an in-process wineserver can run on the console.
         */
        PwWinePlatformReport wine;
        uint64_t gs_value = 0;
        int rc, os_errno = 0;

        memset(&wine, 0, sizeof(wine));
        PS5LOG_LOG("PW_WINE_PLATFORM step=sockets");
        pw_wine_platform_ps5_sockets(&wine);
        PS5LOG_LOG("PW_WINE_PLATFORM socketpair=%d scm_rights=%d sock_errno=%d "
                   "kqueue=%d kevent=%d kq_errno=%d",
                   wine.socketpair_rc, wine.scm_rights_ok, wine.socket_errno,
                   wine.kqueue_rc, wine.kevent_ready, wine.kqueue_errno);
        PS5LOG_LOG("PW_WINE_PLATFORM step=malloc");
        pw_wine_platform_ps5_malloc(&wine);
        PS5LOG_LOG("PW_WINE_PLATFORM malloc_ceiling=%llu limit=%llu",
                   (unsigned long long)wine.malloc_ceiling_bytes,
                   (unsigned long long)wine.malloc_limit_bytes);
        rc = pw_wine_platform_ps5_gsbase_get(&gs_value, &os_errno);
        PS5LOG_LOG("PW_WINE_PLATFORM gs_get=%d errno=%d value=0x%llx", rc, os_errno,
                   (unsigned long long)gs_value);
        rc = pw_wine_platform_ps5_gsbase_same(&os_errno);
        PS5LOG_LOG("PW_WINE_PLATFORM gs_set_same=%d errno=%d", rc, os_errno);
        PS5LOG_LOG("PW_WINE_PLATFORM step=gsbase");
        pw_wine_platform_ps5_gsbase(&wine);
        PS5LOG_LOG("PW_WINE_PLATFORM gsbase_set=%d errno=%d gs_read=%d gs_thread=%d",
                   wine.gsbase_set_rc, wine.gsbase_errno, wine.gsbase_read_ok,
                   wine.gsbase_thread_ok);
        PS5LOG_LOG("PW_WINE_PLATFORM step=sigsegv");
        pw_wine_platform_ps5_sigsegv(&wine);
        PS5LOG_LOG("PW_WINE_PLATFORM sigsegv_rc=%d recovered=%d altstack=%d rip_edit=%d hits=%d "
                   "rip_offset=%d header_offset=%d measured_edit=%d",
                   wine.sigsegv_rc, wine.sigsegv_recovered, wine.sigsegv_on_altstack,
                   wine.sigsegv_rip_edit_ok, wine.sigsegv_hits, wine.sigsegv_rip_offset,
                   wine.sigsegv_header_offset, wine.sigsegv_measured_edit_ok);
        PS5LOG_LOG("PW_WINE_PLATFORM done");
        {
            /* Where each register lives in this console's signal ucontext. */
            static const char *const names[PW_UC_COUNT] = {
                "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10",
                "r11", "r12", "r13", "r14", "r15", "rsp", "rip" };
            PwUcontextMap map;
            int status = pw_ucontext_map_ps5(&map);

            PS5LOG_LOG("PW_UCONTEXT status=%d signal=%d scanned=%d duplicates=%d "
                       "header_size=%d header_rax=%d header_rsp=%d header_rip=%d",
                       status, map.signal, map.scanned, map.duplicates, map.header_size,
                       map.header_rax, map.header_rsp, map.header_rip);
            for (unsigned r = 0; r < PW_UC_COUNT; r++)
                PS5LOG_LOG("PW_UCONTEXT reg=%s offset=%d", names[r], map.offset[r]);
            {
                PwFsGsBaseProbe fsgs;
                pw_fsgsbase_ps5_probe(&fsgs);
                PS5LOG_LOG("PW_FSGSBASE rdfsbase=%d rdgsbase=%d wrgsbase=%d fs=0x%llx gs=0x%llx",
                           fsgs.rdfsbase, fsgs.rdgsbase, fsgs.wrgsbase,
                           (unsigned long long)fsgs.fs_value, (unsigned long long)fsgs.gs_value);
            }
            {
                PwVmSpaceReport vm;
                pw_vmspace_ps5_probe(&vm);
                PS5LOG_LOG("PW_VMSPACE page=%ld low_free=%llu ranges=%u largest=0x%llx+%llu "
                           "largest_single=%d gran64k=%d gran4k=%d errno=%d",
                           vm.page_size, (unsigned long long)vm.low_free_bytes, vm.range_count,
                           (unsigned long long)vm.largest_run_base,
                           (unsigned long long)vm.largest_run_bytes, vm.largest_single_ok,
                           vm.granularity_64k_ok, vm.granularity_4k_ok, vm.first_errno);
                for (uint32_t i = 0; i < vm.range_count; i++)
                    PS5LOG_LOG("PW_VMSPACE range=0x%llx+%llu",
                               (unsigned long long)vm.ranges[i].base,
                               (unsigned long long)vm.ranges[i].bytes);
                PS5LOG_LOG("PW_VMSPACE done");
            }
        }
    }

    {
        /*
         * The argument matrix first. A single EINVAL cannot distinguish an
         * unavailable operation from a malformed call, and an earlier
         * version of this probe read one as a platform refusal when its own
         * arguments were wrong.
         */
        PwLdtAttempt attempts[PW_LDT_ATTEMPTS];
        uint32_t attempt_count = 0u;

        if (pw_compat32_ps5_diagnose(attempts, PW_LDT_ATTEMPTS,
                                     &attempt_count) == PW_OK) {
            for (uint32_t index = 0; index < attempt_count; ++index)
                PS5LOG_LOG("PW_LDT_TRY label=%s op=%d start=0x%x num=%u "
                           "rc=%d errno=%d", attempts[index].label,
                           attempts[index].op, attempts[index].start,
                           attempts[index].num, attempts[index].rc,
                           attempts[index].os_errno);
        }
    }

    if (pw_compat32_ps5_platform(&compat32_state, &compat32_platform) ==
        PW_OK) {
        PwCompat32Report compat32;
        PwGateReport *probe_report = report;
        const int compat_status =
            pw_compat32_probe(&compat32_platform, PW_COMPAT32_TRANSFER,
                              &compat32);

        probe_report->line_count = 0u;
        probe_report->truncated = 0u;
        probe_report->sink = emit_line;
        probe_report->sink_context = NULL;
        (void)pw_gate_compat32(probe_report, &compat32);
        PS5LOG_LOG("PW_COMPAT32_PLATFORM status=%s bases_tried=%u "
                   "chosen_base=0x%x last_errno=%d transfer_build=%d",
                   pw_result_name(compat_status),
                   compat32_state.attempted_bases,
                   compat32_state.chosen_base, compat32_state.last_errno,
                   PW_COMPAT32_TRANSFER);
        probe_report->line_count = 0u;
        probe_report->truncated = 0u;
    }

    (void)pw_file_ps5_provider(&files, &provider);
    /*
     * The first hardware run stopped between the probe above and the
     * loader below, with nothing to say which call did it. These markers
     * bound each remaining step; the backend one is emitted before
     * pw_vm_posix_backend(), whose page_bytes() is the first sysconf() call
     * in the program and an import this firmware has never exercised.
     */
    PS5LOG_LOG("PW_STEP name=backend");
    status = pw_vm_posix_backend(&backend);
    if (status != PW_OK) {
        PS5LOG_LOG("PW_ABORT stage=backend status=%s",
                   pw_result_name(status));
        ps5log_close("pe-map-backend-unavailable");
        _exit(0);
    }

    PS5LOG_LOG("PW_STEP name=backend-ready page_bytes=%llu capabilities=0x%x",
               (unsigned long long)backend.page_bytes, backend.capabilities);

    PS5LOG_LOG("PW_STEP name=root-open");
    {
        PwExecProbe execution;
        PS5LOG_LOG("PW_STEP name=call6-begin kind=synthetic-code");
        status = pw_exec_probe(&backend, &execution);
        PS5LOG_LOG("PW_CALL6 kind=synthetic-code status=%s constant=%llu "
                   "alignment=%llu weighted=%llu high=%llu sealed=%u released=%u",
                   pw_result_name(status),
                   (unsigned long long)execution.constant,
                   (unsigned long long)execution.alignment,
                   (unsigned long long)execution.weighted,
                   (unsigned long long)execution.high,
                   execution.sealed, execution.released);
        if (status != PW_OK) {
            ps5log_close("call6-failed");
            _exit(1);
        }
    }
    status = provider.open(provider.context, root_name, &root);
    if (status != PW_OK) {
        PS5LOG_LOG("PW_ABORT stage=root status=%s name=%s",
                   pw_result_name(status), root_name);
        ps5log_close("pe-map-root-unreadable");
        _exit(0);
    }

    memset(&request, 0, sizeof(request));
    request.root_bytes = root.bytes;
    request.root_size = root.size;
    request.root_name = root_name;
    request.provider_path = stage_dir;
    request.sink = emit_line;
    request.sink_context = NULL;

    {
        /*
         * A stack anchor. The first crash faulted with the fault address
         * equal to rsp and SEGV_MAPERR, which is the stack pointer standing
         * on an unmapped page; comparing this against the rsp in PW_SIGNAL
         * says how far the stack actually fell before it ran out.
         */
        const int anchor = 0;

        PS5LOG_LOG("PW_STEP name=gate root_bytes=%llu stack_anchor=%p "
                   "loader=%p report=%p",
                   (unsigned long long)root.size, (const void *)&anchor,
                   (void *)loader, (void *)report);
    }
    /* Records reach the log through the sink as they are produced, so
     * nothing is emitted here: a crash mid-gate must not cost the evidence
     * that was already gathered. */
    status = pw_gate_run(report, loader, &provider, &backend, &request);

    provider.close(provider.context, &root);
    PS5LOG_LOG("PW_FILES opens=%u closes=%u failures=%u bytes=%llu",
               files.opens, files.closes, files.failures,
               (unsigned long long)files.bytes_read);

    /*
     * Every span the provider handed out has been closed and every
     * reservation released before the channel closes, so the transcript
     * ends on a state the operator can trust.
     */
    ps5log_close(status == PW_OK ? "pe-map-complete" : "pe-map-failed");
    _exit(0);
}
