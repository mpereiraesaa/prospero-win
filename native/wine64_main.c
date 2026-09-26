/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Native entry for running Wine's own Unix side inside the title.
 *
 * The title requests the /data mount, loads ntdll.prx, prepares Wine's
 * environment and enters __wine_main on a dedicated thread
 * (src/pw_wine_start.c). Wine's stderr reaches ps5log/1 through the sink
 * ntdll exports (Wine patch 0560), so its debug channels become telemetry;
 * the main thread shows the frames Wine's user driver presents (patch 0400)
 * on VideoOut, turns DualSense buttons into Wine key events, and reports
 * Wine's address-space counters until Wine exits the process or the run
 * deadline passes.
 */
#include "ps5log/ps5log.h"
#include "../src/pw_wine_start.h"
#include "pw_data_mount.h"
#include "pw_pad_ps5.h"
#include "pw_videoout_ps5.h"
#include "pw_wine_display.h"
#include "../include/prospero_win.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PW_TITLE_ID "PPSA99995"
#define PW_APP_NAME "prospero-win-wine64"

/* Where the title's Wine runtime can be seen: /app0 inside the sandbox;
 * once /data is granted the process sees the real root, where /app0 does
 * not exist (measured), and the sandbox's view of app0 is used instead. */
#define PW_WINE64_RUNTIME "/win/wine/lib/wine/x86_64-unix"
#define PW_SANDBOX_APP0 "/mnt/sandbox/" PW_TITLE_ID "_000/app0"
static const char *const runtime_roots[] = { "/app0", PW_SANDBOX_APP0 };
#ifndef PW_WINE64_PREFIX
#define PW_WINE64_PREFIX "/download0/prospero-win/prefix"  /* sandbox fallback */
#endif
#ifndef PW_WINE64_PREFIX_DATA
#define PW_WINE64_PREFIX_DATA "/data/prospero-win/prefix"  /* when /data is granted */
#endif
#ifndef PW_WINE64_EXE
#define PW_WINE64_EXE "C:\\Games\\Pinball\\PINBALL.EXE"
#endif
#ifndef PW_WINE64_DEBUG
/* No +seh: WoW64 callback returns unwind with 80000026 many times a second
 * and would flood the sink. */
#define PW_WINE64_DEBUG "err+all,+loaddll,+process"
#endif
#ifndef PW_WINE64_SECONDS
/* Pinball presents its first frame about 31 s in (run 13). */
#define PW_WINE64_SECONDS 120
#endif

/* The largest desktop shown: Wine's PS5 driver defaults to 800x600
 * (WINE_PS5_DESKTOP overrides); bigger frames are counted and dropped. */
enum { PW_WINE64_MAX_FRAME = 1280 * 1024 * 4, PW_WINE64_TICK_US = 16000 };

/* Pinball's keys, as the direct runtime maps them (native/runtime_main.c). */
enum { PAD_UP = 0x10u, PAD_RIGHT = 0x20u, PAD_LEFT = 0x80u, PAD_L1 = 0x400u, PAD_R1 = 0x800u,
       PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u, PAD_OPTIONS = 0x8u };
static const PwPadKeyMap pinball_pad_map[] = {
    { PAD_L1, 'Z', 0, 0, "left-flipper" },
    { PAD_R1, 0xbf, 0, 0, "right-flipper" },
    { PAD_CROSS, 0x20, 0, 0, "plunger" },
    { PAD_LEFT, 'X', 0, 0, "nudge-left" },
    { PAD_RIGHT, 0xbe, 0, 0, "nudge-right" },
    { PAD_UP, 0x26, 0x48, 1, "nudge-up" },
    { PAD_OPTIONS, 0x72, 0, 0, "pause" },
    { PAD_SQUARE, 0x71, 0, 0, "new-game" },
};

int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                                 uint32_t flags, const void *option, int *result);
int sceKernelGetModuleInfo(int32_t handle, void *info);

static uint64_t now_ns(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0u;
    return (uint64_t)value.tv_sec * 1000000000u + (uint64_t)value.tv_nsec;
}

/* ---- Wine stderr -> ps5log, one line per text line -------------------- */

static char sink_line[1024];
static size_t sink_used;
static pthread_mutex_t sink_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long sink_calls;

static void wine_output_sink(const char *str, size_t len)
{
    sink_calls++;
    pthread_mutex_lock(&sink_lock);
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n' || sink_used == sizeof(sink_line) - 1) {
            sink_line[sink_used] = 0;
            PS5LOG_LOG("WINE %s", sink_line);
            sink_used = 0;
            if (str[i] == '\n') continue;
        }
        sink_line[sink_used++] = str[i];
    }
    pthread_mutex_unlock(&sink_lock);
}

/* ---- frames and input -------------------------------------------------- */

/* Wine presents on its own threads: keep a copy for the main thread. The
 * two frame buffers are mapped, not static: the title image must stay clear
 * of the low range PE32 executables occupy, and libc's heap is too small. */
static PwWineFrameBox frames;
static uint8_t *frame_shown;

static int wine_present(void *context, const void *bgra, uint32_t width, uint32_t height,
                        uint32_t stride)
{
    (void)context;
    return pw_wine_frame_box_put(&frames, bgra, width, height, stride);
}

/* ---- fd 2 -> ps5log ---------------------------------------------------- */

/* The in-process wineserver writes its errors with fprintf(stderr), which a
 * title does not show: fd 2 becomes one end of a socket pair (pipe() is not
 * available and dup2 onto fd 2 is refused, measured) whose lines a thread
 * forwards as "WINESERVER ..." records. */
static int fd2_reader = -1;

static void *forward_fd2(void *arg)
{
    char buffer[1024];
    size_t used = 0;

    (void)arg;
    for (;;) {
        ssize_t got = read(fd2_reader, buffer + used, sizeof(buffer) - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
        for (;;) {
            char *newline = memchr(buffer, '\n', used);
            if (!newline && used < sizeof(buffer) - 1) break;
            size_t line = newline ? (size_t)(newline - buffer) : used;
            buffer[line] = 0;
            PS5LOG_LOG("WINESERVER %s", buffer);
            line += newline ? 1u : 0u;
            memmove(buffer, buffer + line, used - line);
            used -= line;
        }
    }
    return NULL;
}

/* Close fd 2 first: descriptors are allocated lowest first, so one end of
 * the new pair takes number 2; keep that one as the writer. */
static int capture_fd2(void)
{
    int pair[2];
    pthread_t thread;

    close(2);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
    if (pair[1] == 2) fd2_reader = pair[0];
    else if (pair[0] == 2) {
        fd2_reader = dup(pair[0]);  /* free number 2 for the writer */
        if (fd2_reader < 0 || dup2(pair[1], 2) != 2) return -2;
    } else {
        /* 0 and 1 were free too: move the writer onto the closed 2 */
        fd2_reader = pair[0];
        if (dup2(pair[1], 2) != 2) return -3;
    }
    return pthread_create(&thread, NULL, forward_fd2, NULL) ? -4 : 0;
}

/* Wine ends the process with exit(): close the log so it is complete. */
static void on_exit_report(void)
{
    PS5LOG_LOG("PW_WINE64 exit sink_calls=%lu", sink_calls);
    ps5log_close("wine64-exit");
}

/* ---- early fault report ------------------------------------------------ */

/* Until Wine installs its own handlers, a fault inside ntdll would end the
 * process silently; report where it happened relative to ntdll's segments.
 * RIP is read at the measured ucontext offset (224). */
static PwWineStart *fault_start;

static void early_fault(int sig, siginfo_t *info, void *opaque)
{
    uint64_t rip = 0;

    memcpy(&rip, (const uint8_t *)opaque + 224, sizeof(rip));
    PS5LOG_LOG("PW_WINE64 fault sig=%d addr=%p rip=0x%llx", sig, info ? info->si_addr : NULL,
               (unsigned long long)rip);
    for (uint32_t i = 0; fault_start && i < fault_start->segment_count; i++) {
        uint64_t base = (uint64_t)(uintptr_t)fault_start->segments[i].address;
        if (rip >= base && rip < base + fault_start->segments[i].size)
            PS5LOG_LOG("PW_WINE64 fault in ntdll segment %u offset=0x%llx", i,
                       (unsigned long long)(rip - base));
    }
    ps5log_close("wine64-early-fault");
    _exit(3);
}

static void install_early_fault_report(PwWineStart *start)
{
    struct sigaction action;

    fault_start = start;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = early_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
}

/* ---- PS5 bindings for the start sequence ------------------------------ */

static int set_env(const char *name, const char *value)
{
    return setenv(name, value, 1);
}

struct thread_start { void (*entry)(void *); void *arg; };

static void *thread_trampoline(void *opaque)
{
    struct thread_start start = *(struct thread_start *)opaque;

    free(opaque);
    PS5LOG_LOG("PW_WINE64 thread entered, calling __wine_main");
    start.entry(start.arg);
    PS5LOG_LOG("PW_WINE64 __wine_main returned");
    return NULL;
}

static int start_thread(void (*entry)(void *), void *arg, size_t stack_bytes)
{
    pthread_attr_t attr;
    pthread_t thread;
    struct thread_start *start = malloc(sizeof(*start));
    int status;

    if (!start) return -1;
    start->entry = entry;
    start->arg = arg;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    status = pthread_create(&thread, &attr, thread_trampoline, start);
    pthread_attr_destroy(&attr);
    if (status) free(start);
    return status;
}

int main(int argc, char **argv)
{
    static const PwWineStartEnv extra[] = {
        { "WINEDEBUG", PW_WINE64_DEBUG },
        /* the i386 exe runs in this process through WoW64; otherwise Wine
         * starts it from start.exe in a new process, which a title cannot */
        { "WINEARCH", "wow64" },
        { "HOME", PW_WINE64_PREFIX },
        { "USER", "prospero" },
        { "WINE_PS5_TRACE_STARTUP", "1" },  /* patch 0560: name startup steps */
    };
    static const char *const wine_argv[] = { "wine", PW_WINE64_EXE };
    static char ntdll_dir[256], ntdll_path[288];
    static const PwWineStartOps ops = {
        sceKernelLoadStartModule, sceKernelGetModuleInfo, set_env, start_thread };
    static PwWineStartConfig config = {
        .ntdll_path = ntdll_path,
        .ntdll_dir = ntdll_dir,
        .prefix = PW_WINE64_PREFIX,
        .extra_env = extra, .extra_env_count = sizeof(extra) / sizeof(extra[0]),
        .argc = 2, .argv = wine_argv, .stack_bytes = 16u << 20,
    };
    static PwWineStart start;
    static PwVideoOutPs5 video;
    static PwPadPs5 pad;
    int (*post_input)(const PwWineInput *) = NULL;
    uint64_t shown_sequence = 0, shown = 0, posted = 0, refused = 0;
    ps5log_config log_config;
    PwPadPs5Ops pad_ops;
    int status, video_status = PW_ERR_STATE, pad_status = PW_ERR_STATE;

    (void)argc; (void)argv;
    ps5log_config_defaults(&log_config);
    if (ps5log_load_config(ps5log_default_conf_paths, ps5log_default_conf_path_count,
                           &log_config, NULL) == 0)
        ps5log_init(&log_config, PW_TITLE_ID, PW_APP_NAME, now_ns());

    /* Request /data; keep the /download0 prefix if it does not appear. */
    {
        PwDataMountResult mount;
        if (pw_data_mount_request(&mount) == 0) config.prefix = PW_WINE64_PREFIX_DATA;
        PS5LOG_LOG("PW_WINE64 data_mount data_before=%d wrote=%d write_errno=%d data_after=%d "
                   "waited_ms=%d prefix=%s",
                   mount.data_before, mount.wrote_request, mount.write_errno,
                   mount.data_after, mount.waited_ms, config.prefix);
    }
    for (size_t i = 0; i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
        struct stat st;
        snprintf(ntdll_dir, sizeof(ntdll_dir), "%s" PW_WINE64_RUNTIME, runtime_roots[i]);
        snprintf(ntdll_path, sizeof(ntdll_path), "%s/ntdll.prx", ntdll_dir);
        if (stat(ntdll_path, &st) == 0) break;
        PS5LOG_LOG("PW_WINE64 runtime not at %s", ntdll_dir);
    }
    PS5LOG_LOG("PW_WINE64 ntdll=%s prefix=%s exe=%s", config.ntdll_path, config.prefix,
               PW_WINE64_EXE);
    status = capture_fd2();
    if (status != 0) PS5LOG_LOG("PW_WINE64 fd2_capture=failed status=%d", status);

    status = pw_wine_start_load(&start, &config, &ops);
    PS5LOG_LOG("PW_WINE64 load status=%d stage=%d module=0x%x segments=%u module_start=%d "
               "adopted=%d stats=%d", status, start.stage, (unsigned)start.module,
               start.segment_count, start.module_start_result, start.adopted != NULL,
               start.virtual_stats != NULL);
    install_early_fault_report(&start);
    atexit(on_exit_report);
    if (status == PW_OK) {
        void (*set_sink)(void (*)(const char *, size_t)) = (void (*)(void (*)(const char *, size_t)))
            (uintptr_t)pw_prx_lookup(start.descriptor, "__wine_ps5_set_output_sink");
        PS5LOG_LOG("PW_WINE64 output_sink=%d", set_sink != NULL);
        if (set_sink) set_sink(wine_output_sink);
        void (*set_present)(PwWinePresentSink, void *) = (void (*)(PwWinePresentSink, void *))
            (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_present_sink");
        post_input = (int (*)(const PwWineInput *))
            (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_post_input");
        video_status = pw_videoout_ps5_open(&video);
        pad_status = pw_pad_ps5_platform_ops(&pad_ops);
        if (pad_status == PW_OK)
            pad_status = pw_pad_ps5_open(&pad, &pad_ops, pinball_pad_map,
                                         sizeof(pinball_pad_map) / sizeof(pinball_pad_map[0]));
        {
            uint8_t *storage = mmap(NULL, 2u * PW_WINE64_MAX_FRAME, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0);
            if (storage != MAP_FAILED &&
                pw_wine_frame_box_init(&frames, storage, PW_WINE64_MAX_FRAME) == 0) {
                frame_shown = storage + PW_WINE64_MAX_FRAME;
                if (set_present) set_present(wine_present, NULL);
            }
        }
        PS5LOG_LOG("PW_WINE64 display present_sink=%d frames=%d post_input=%d video=%s pad=%s",
                   set_present != NULL, frame_shown != NULL, post_input != NULL,
                   pw_result_name(video_status),
                   pw_result_name(pad_status));
        status = pw_wine_start_environment(&start, &config, &ops);
        PS5LOG_LOG("PW_WINE64 environment status=%d", status);
    }
    if (status == PW_OK) {
        status = pw_wine_start_run(&start, &config, &ops);
        PS5LOG_LOG("PW_WINE64 run status=%d", status);
    }
    for (uint64_t tick = 1, deadline = now_ns() + (uint64_t)PW_WINE64_SECONDS * 1000000000u;
         status == PW_OK && now_ns() < deadline; tick++) {
        PwGdiTargetView view;
        PwWineInput events[2 * sizeof(pinball_pad_map) / sizeof(pinball_pad_map[0])];
        int presented = 0;

        if (pad_status == PW_OK && post_input && pw_pad_ps5_read(&pad) == PW_OK) {
            size_t count = pw_wine_pad_inputs(&pad.core, events, sizeof(events) / sizeof(events[0]));
            for (size_t i = 0; i < count; i++) {
                if (post_input(&events[i]) == 0) posted++;
                else refused++;
            }
        }
        if (video_status == PW_OK && frame_shown &&
            pw_wine_frame_box_take(&frames, &shown_sequence, frame_shown, PW_WINE64_MAX_FRAME,
                                   &view) == 1 &&
            pw_videoout_ps5_present(&video, &view) == PW_OK) {
            shown++;          /* present waits for the vblank */
            presented = 1;
        }
        if (!presented) usleep(PW_WINE64_TICK_US);
        if (tick % 60 == 0) {
            uint64_t v[16] = { 0 };

            PS5LOG_LOG("PW_WINE64 alive tick=%llu sink_calls=%lu frames_put=%llu shown=%llu "
                       "rejected=%llu last=%ux%u inputs=%llu refused=%llu",
                       (unsigned long long)tick, sink_calls, (unsigned long long)frames.sequence,
                       (unsigned long long)shown, (unsigned long long)frames.rejected,
                       frames.width, frames.height, (unsigned long long)posted,
                       (unsigned long long)refused);
            if (start.virtual_stats && tick % 300 == 0) {
                start.virtual_stats(v, 16);
                PS5LOG_LOG("PW_WINE64 mmap=%llu munmap=%llu mprotect=%llu skipped=%llu "
                           "faults=%llu images=%llu",
                           (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
                           (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6]);
            }
        }
    }
    PS5LOG_LOG("PW_WINE64 done status=%d stage=%d", status, start.stage);
    ps5log_close(status == PW_OK ? "wine64-deadline" : "wine64-start-failed");
    _exit(status == PW_OK ? 0 : 1);
}
