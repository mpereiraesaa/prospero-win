/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Bounded native Wine bootstrap for the generated PE32 application fixture. */
#include "pw_file_ps5.h"
#include "ps5log/ps5log.h"
#include "../src/pw_app_profile.h"
#include "../src/pw_vm_posix.h"
#include "../src/pw_wine_gate.h"
#include "../src/pw_wine_runner.h"
#include "../src/pw_wine_seed_services.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#define PW_TITLE_ID "PPSA99995"
#define PW_APP_NAME "prospero-win-wine-bootstrap"
#ifndef PW_STAGE_DIR
#define PW_STAGE_DIR "/app0/win"
#endif
#ifndef PW_ROOT_MODULE
#define PW_ROOT_MODULE "app.exe"
#endif
#ifndef PW_USE_APP_PROFILE
#define PW_USE_APP_PROFILE 0
#endif
#ifndef PW_WINE_VERSION
#define PW_WINE_VERSION "11.17"
#endif
#ifndef PW_DBT_CHAINING
#define PW_DBT_CHAINING 1
#endif
#ifndef PW_DBT_RESIDENCY
#define PW_DBT_RESIDENCY 1
#endif
#ifndef PW_DBT_LAZY_FLAGS
#define PW_DBT_LAZY_FLAGS 1
#endif

static PwFilePs5 files;
static PwFileProvider provider;
static PwWineFileService file_service;
static PwWineSeedServices seed_services;
static PwVmBackend backend;
static char wine_version_info[4u * 128u];
static uint32_t wine_version_info_bytes;

static uint64_t now_ns(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0u;
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

static void *reserve_scratch(size_t bytes)
{
    void *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return memory == MAP_FAILED ? NULL : memory;
}

static int append_field(size_t *used, const char *field)
{
    const size_t length = strlen(field) + 1u;

    if (*used + length > sizeof(wine_version_info))
        return 0;
    memcpy(wine_version_info + *used, field, length);
    *used += length;
    return 1;
}

/* Same four-string block ntdll receives from Wine's version query. The pinned
 * manifest supplies the Wine version; the platform fields identify the PS5
 * host without claiming a kernel release the title SDK does not expose. */
static int build_version_info(void)
{
    char version[64];
    char build[80];
    size_t used = 0u;

    if (snprintf(version, sizeof(version), "%s", PW_WINE_VERSION) >=
            (int)sizeof(version) ||
        snprintf(build, sizeof(build), "wine-%s", PW_WINE_VERSION) >=
            (int)sizeof(build))
        return 0;
    if (!append_field(&used, version) || !append_field(&used, build) ||
        !append_field(&used, "FreeBSD") || !append_field(&used, "PS5"))
        return 0;
    wine_version_info_bytes = (uint32_t)used;
    return 1;
}

static int debug_write(void *context, const void *bytes, uint32_t length)
{
    (void)context;
    (void)bytes;
    /* Do not forward Wine debug text to telemetry; record only bounded size. */
    PS5LOG_LOG("PW_WINE_DEBUG_WRITE bytes=%u", length);
    return 0;
}

static int load_app_profile(PwFilePs5 *state, uint8_t *buffer,
                            PwAppProfile *profile)
{
    uint32_t handle = 0u;
    uint32_t total = 0u;
    int status = pw_file_ps5_stream_open(state, "app.profile", "rb", &handle);

    if (status != PW_OK)
        return status;
    while (total < PW_APP_PROFILE_MAX_BYTES) {
        uint32_t received = 0u;

        status = pw_file_ps5_stream_read(state, handle, buffer + total,
            PW_APP_PROFILE_MAX_BYTES - total, &received);
        if (status != PW_OK || received == 0u)
            break;
        total += received;
    }
    if (status == PW_OK && total == PW_APP_PROFILE_MAX_BYTES) {
        uint8_t extra;
        uint32_t received = 0u;

        status = pw_file_ps5_stream_read(state, handle, &extra, 1u,
                                         &received);
        if (status == PW_OK && received != 0u)
            status = PW_ERR_LIMIT;
    }
    {
        const int close_status = pw_file_ps5_stream_close(state, handle);

        if (status == PW_OK && close_status != PW_OK)
            status = close_status;
    }
    if (status != PW_OK)
        return status;
    return pw_app_profile_parse(buffer, total, profile);
}

static int profile_application_directory(const PwAppProfile *profile,
                                         char *output, size_t capacity)
{
    const char *separator;
    size_t length;

    if (!profile || !output || capacity == 0u)
        return PW_ERR_PRECONDITION;
    separator = strrchr(profile->executable, '\\');
    if (!separator)
        return PW_ERR_MALFORMED;
    length = (size_t)(separator - profile->executable) + 1u;
    if (length == 0u || length >= capacity)
        return PW_ERR_LIMIT;
    memcpy(output, profile->executable, length);
    output[length] = '\0';
    return PW_OK;
}

static void fatal_signal(int number, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;

    PS5LOG_LOG("PW_WINE_SIGNAL sig=%d code=%d addr=%p pc=%p rsp=%p",
        number, info ? info->si_code : 0, info ? info->si_addr : NULL,
        uc ? (void *)(uintptr_t)uc->uc_mcontext.mc_rip : NULL,
        uc ? (void *)(uintptr_t)uc->uc_mcontext.mc_rsp : NULL);
    ps5log_close("wine-bootstrap-signal");
    _exit(1);
}

static void install_signals(void)
{
    static const int signals[] = {
        SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS,
    };
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fatal_signal;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]);
         ++index)
        (void)sigaction(signals[index], &action, NULL);
}

int main(int argc, char **argv)
{
    static const char *const runtime_dir =
        PW_STAGE_DIR "/runtime/lib/i386-windows";
    static const char *const nls_dir = PW_STAGE_DIR "/runtime/nls";
    ps5log_config log_config;
    PwFilePs5Smoke smoke;
    PwWineRunner *runner;
    PwWineGateReport *report;
    PwWineGateConfig config;
    PwAppProfile app_profile;
    uint8_t *profile_bytes = NULL;
    char root_module[PW_APP_PATH_CAPACITY] = PW_ROOT_MODULE;
    char application_directory[PW_APP_PATH_CAPACITY] = "C:\\";
    char command_line[PW_APP_ARGUMENTS_CAPACITY];
    const char *modules[3];
    PwWineDebugSink debug_sink = {.context = NULL, .write = debug_write};
    int log_config_status;
    int log_status;
    int status;

    (void)argc;
    (void)argv;
    ps5log_config_defaults(&log_config);
    log_config_status = ps5log_load_config(ps5log_default_conf_paths,
        ps5log_default_conf_path_count, &log_config, NULL);
    log_status = log_config_status == 0
        ? ps5log_init(&log_config, PW_TITLE_ID, PW_APP_NAME, now_ns())
        : log_config_status;
    install_signals();
    memset(&app_profile, 0, sizeof(app_profile));

    runner = reserve_scratch(sizeof(*runner));
    report = reserve_scratch(sizeof(*report));
    if (!runner || !report) {
        PS5LOG_LOG("PW_WINE_ABORT stage=scratch runner=%d report=%d",
                   runner != NULL, report != NULL);
        ps5log_close("wine-bootstrap-no-scratch");
        _exit(1);
    }
    PS5LOG_LOG("PW_WINE_BEGIN schema=1 stage=%s root=%s log_config=%d "
               "log_init=%d runner_bytes=%llu report_bytes=%llu",
               PW_STAGE_DIR, root_module, log_config_status, log_status,
               (unsigned long long)sizeof(*runner),
               (unsigned long long)sizeof(*report));

    status = pw_file_ps5_init(&files, PW_STAGE_DIR "/app");
    if (status == PW_OK)
        status = pw_file_ps5_set_runtime_directory(&files, runtime_dir);
    if (status == PW_OK)
        status = pw_file_ps5_set_runtime_nls_directory(&files, nls_dir);
    if (status == PW_OK)
        status = pw_file_ps5_provider(&files, &provider);
    if (status == PW_OK)
        status = pw_file_ps5_wine_file_service(&files, &file_service);
    if (status == PW_OK)
        status = pw_vm_posix_backend(&backend);
    if (status != PW_OK) {
        PS5LOG_LOG("PW_WINE_ABORT stage=platform-init status=%s",
                   pw_result_name(status));
        ps5log_close("wine-bootstrap-init-failed");
        _exit(1);
    }

    if (PW_USE_APP_PROFILE) {
        profile_bytes = reserve_scratch(PW_APP_PROFILE_MAX_BYTES);
        if (!profile_bytes) {
            PS5LOG_LOG("PW_WINE_ABORT stage=profile-buffer");
            ps5log_close("wine-bootstrap-profile-buffer");
            _exit(1);
        }
        status = load_app_profile(&files, profile_bytes, &app_profile);
        if (status == PW_OK && app_profile.architecture != PW_APP_ARCH_PE32)
            status = PW_ERR_UNSUPPORTED;
        if (status == PW_OK && app_profile.graphics != PW_APP_GRAPHICS_GDI)
            status = PW_ERR_UNSUPPORTED;
        if (status == PW_OK)
            status = pw_app_profile_stage_name(&app_profile, root_module,
                                               sizeof(root_module));
        if (status == PW_OK)
            status = profile_application_directory(&app_profile,
                application_directory, sizeof(application_directory));
        if (status == PW_OK)
            status = pw_app_profile_build_command_line(&app_profile,
                command_line, sizeof(command_line));
        if (status != PW_OK) {
            PS5LOG_LOG("PW_WINE_ABORT stage=profile status=%s",
                       pw_result_name(status));
            ps5log_close("wine-bootstrap-profile-invalid");
            _exit(1);
        }
        PS5LOG_LOG("PW_WINE_PROFILE id=%s runtime=%s prefix=%s exe=%s "
                   "cwd=%s args=%u architecture=pe32 graphics=%u",
                   app_profile.id, app_profile.runtime, app_profile.prefix,
                   root_module, app_profile.working_directory,
                   (unsigned)(app_profile.arguments[0] != '\0'),
                   (unsigned)app_profile.graphics);
    }

    modules[0] = root_module;
    modules[1] = "ntdll.dll";
    modules[2] = "kernelbase.dll";

    status = pw_file_ps5_smoke(&files, root_module, &smoke);
    PS5LOG_LOG("PW_WINE_FS_SMOKE status=%s open=%d stat=%d read=%d seek=%d "
               "close=%d size=%lld magic=0x%02x%02x is_pe=%d",
               pw_result_name(status), smoke.open_result, smoke.stat_result,
               smoke.read_result, smoke.seek_result, smoke.close_result,
               smoke.size, smoke.first_bytes[0], smoke.first_bytes[1],
               smoke.is_pe);
    if (status != PW_OK) {
        ps5log_close("wine-bootstrap-app-missing");
        _exit(1);
    }
    if (!build_version_info()) {
        PS5LOG_LOG("PW_WINE_ABORT stage=version-info");
        ps5log_close("wine-bootstrap-version-unavailable");
        _exit(1);
    }

    pw_wine_seed_services_init(&seed_services);
    pw_wine_runner_init(runner);
    memset(&config, 0, sizeof(config));
    config.provider = &provider;
    config.backend = &backend;
    config.runner = runner;
    config.files = &file_service;
    config.registry = pw_wine_seed_registry(&seed_services);
    config.objects = pw_wine_seed_objects(&seed_services);
    config.wine_version_info = wine_version_info;
    config.wine_version_info_bytes = wine_version_info_bytes;
    config.token_user_sid = pw_wine_seed_user_sid(
        &config.token_user_sid_bytes);
    config.root_module = root_module;
    config.entry_module = "ntdll.dll";
    config.entry_symbol = "LdrInitializeThunk";
    config.module_count = sizeof(modules) / sizeof(modules[0]);
    for (uint32_t index = 0; index < config.module_count; ++index)
        config.modules[index] = modules[index];
    config.root_application = 1u;
    if (PW_USE_APP_PROFILE) {
        config.process_image_path = app_profile.executable;
        config.process_current_directory = app_profile.working_directory;
        config.process_application_directory = application_directory;
        config.process_command_line = command_line;
    }
    config.bridge_calls = 1u;
    config.unixlib_calls = 1u;
    config.debug_sink = &debug_sink;
    config.call_budget = PW_WINE_GATE_DEFAULT_CALLS;
    config.modes_set = 1u;
    config.chaining = PW_DBT_CHAINING;
    config.residency = PW_DBT_RESIDENCY;
    config.lazy_flags = PW_DBT_LAZY_FLAGS;

    memset(report, 0, sizeof(*report));
    status = pw_wine_gate_run(&config, report);
    PS5LOG_LOG("PW_WINE_RUN status=%s stop=%s stage=%s retired=%llu "
               "dispatches=%llu blocks=%llu modes=%u,%u,%u calls=%u "
               "unixlib=%llu entry_eip=0x%x entry_reached=%u "
               "app_exit=0x%x app_call=0x%x host_calls=%llu",
               pw_result_name(status), pw_wine_stop_name(report->stop),
               report->gate_stage ? report->gate_stage : "none",
               (unsigned long long)report->retired,
               (unsigned long long)report->dispatches,
               (unsigned long long)report->translated_blocks,
               report->chaining, report->residency, report->lazy_flags,
               report->calls_serviced,
               (unsigned long long)report->unixlib.serviced,
               report->main_entry_eip, report->main_entry_reached,
               report->exit_status, report->exit_call_id,
               (unsigned long long)report->host_calls);
    for (uint32_t index = 0; index < report->module_count; ++index) {
        const PwWineModuleRecord *module = &report->modules[index];

        PS5LOG_LOG("PW_WINE_MODULE name=%s sha256=%s size=%u machine=0x%04x "
                   "loaded=%u runtime=%u tls=%u",
                   module->name, module->sha256, module->size,
                   module->machine, module->loaded, module->runtime,
                   module->tls_present);
    }
    PS5LOG_LOG("PW_WINE_SERVICES files=%llu reads=%llu bytes=%llu "
               "registry=%llu/%llu/%llu objects=%llu nls=%llu/%llu "
               "cleanup=%u/%u/%u/%u",
               (unsigned long long)report->file_opens,
               (unsigned long long)report->file_reads,
               (unsigned long long)report->file_bytes,
               (unsigned long long)report->key_opens,
               (unsigned long long)report->key_queries,
               (unsigned long long)report->key_sets,
               (unsigned long long)report->object_opens,
               (unsigned long long)report->nls_maps,
               (unsigned long long)report->nls_refusals,
               report->cleanup_modules, report->cleanup_mappings,
               report->cleanup_translations, report->cleanup_failures);

    pw_wine_runner_release(runner);
    const int expected_stop =
        report->stop == PW_WINE_STOP_PROCESS_TERMINATED && status != PW_OK;
    ps5log_close(expected_stop || status == PW_OK
                     ? "wine-bootstrap-finished"
                     : "wine-bootstrap-frontier");
    _exit(expected_stop || status == PW_OK ? 0 : 1);
}
