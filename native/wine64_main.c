/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Native entry for running Wine's own Unix side inside the title.
 *
 * The title runs one game per process (src/pw_wine_launch.h). Started with
 * no game it shows the launcher, without loading Wine; choosing a game
 * restarts the title with sceSystemServiceLoadExec and the game's
 * arguments. Holding Options+Create in a game sends it Alt+F4; when Wine
 * exits, or the game does not close in time, the title restarts into the
 * launcher.
 *
 * The launcher and a game request the /data mount, where the library is.
 * A game then loads ntdll.prx, prepares Wine's
 * environment and enters __wine_main on a dedicated thread
 * (src/pw_wine_start.c). Wine's stderr reaches ps5log/1 through the sink
 * ntdll exports (Wine patch 0560), so its debug channels become telemetry;
 * the main thread shows the frames Wine's user driver presents (patch 0400)
 * on VideoOut, turns DualSense buttons into Wine key events, and reports
 * Wine's address-space counters until Wine exits the process or the run
 * deadline passes. Wine's audio driver plays its mix on the console's main
 * audio port through the title's audio sink.
 */
#include "ps5log/ps5log.h"
#include "../src/pw_wine_start.h"
#include "../src/pw_launcher_render.h"
#include "../src/pw_wine_launch.h"
#include "pw_data_mount.h"
#include "pw_audio_ps5.h"
#include "pw_pad_ps5.h"
#include "pw_videoout_ps5.h"
#include "pw_wine_library.h"
#include "../src/pw_present.h"
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
/* The title's data: profiles/, input/, prefix/ and prefixes/<name>. */
#define PW_WINE64_ROOT_DATA "/data/prospero-win"            /* when /data is granted */
#define PW_WINE64_ROOT_SANDBOX "/download0/prospero-win"    /* sandbox fallback */
#ifndef PW_WINE64_DEBUG
/* No +seh: WoW64 callback returns unwind with 80000026 many times a second
 * and would flood the sink. */
#define PW_WINE64_DEBUG "err+all,+loaddll,+process"
#endif
#ifndef PW_WINE64_SECONDS
/* A game closes itself this long after it started; 0 lets it run until it
 * is closed. Pinball presents its first frame about 31 s in (run 13). */
#define PW_WINE64_SECONDS 0
#endif
#ifndef PW_WINE64_SCRIPT
/* 1 drives the launcher unattended for validation: it opens the first game
 * PW_WINE64_SCRIPT_CYCLES times, each closed by the deadline above. */
#define PW_WINE64_SCRIPT 0
#endif
#ifndef PW_WINE64_SCRIPT_CYCLES
#define PW_WINE64_SCRIPT_CYCLES 2
#endif

/* The games, from <root>/profiles (native/pw_wine_library.h); nothing is
 * built in. catalog holds the valid ones for pw_wine_launch. */
static PwWineLibrary library;
static PwWineApp catalog[PW_WINE_LIBRARY_MAX];
static char catalog_detail[PW_WINE_LIBRARY_MAX][64];
static size_t catalog_count;
static const char *library_root = PW_WINE64_ROOT_SANDBOX;

/* The largest desktop shown: Wine's PS5 driver defaults to 800x600
 * (WINE_PS5_DESKTOP overrides); bigger frames are counted and dropped. */
enum { PW_WINE64_MAX_FRAME = 1280 * 1024 * 4, PW_WINE64_TICK_US = 16000,
       PW_WINE64_TICKS_PER_S = 60, PW_WINE64_CLOSE_WAIT_S = 5 };

enum { PAD_CREATE = 0x1u, PAD_OPTIONS = 0x8u, PAD_UP = 0x10u, PAD_RIGHT = 0x20u,
       PAD_DOWN = 0x40u, PAD_LEFT = 0x80u, PAD_L1 = 0x400u, PAD_R1 = 0x800u,
       PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u, PAD_CLOSE = PAD_OPTIONS | PAD_CREATE };
/* The pad core needs a key map to open; the title reads raw edges and maps
 * them with the game's bindings (pw_game_profile). */
static const PwPadKeyMap pad_open_map[] = { { PAD_CROSS, 0x20, 0, 0, "cross" } };


int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                                 uint32_t flags, const void *option, int *result);
int sceKernelGetModuleInfo(int32_t handle, void *info);
int sceKernelAvailableFlexibleMemorySize(size_t *bytes);

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

/* A game presenting with Vulkan scans out through the GPU driver's own video
 * output (patch 0460), which the title must close first. The main thread
 * owns the title's, so the driver's request waits for it to be closed. */
static int display_request, display_closed;

static int wine_release_display(void *context)
{
    (void)context;
    __atomic_store_n(&display_request, 1, __ATOMIC_RELEASE);
    for (int waited = 0; waited < 5000; waited++) {
        if (__atomic_load_n(&display_closed, __ATOMIC_ACQUIRE)) return 0;
        usleep(1000);
    }
    return -1;
}

/* ---- sound -------------------------------------------------------------- */

/* Wine's audio driver (wine/wineps5) mixes every stream a game plays into
 * one grain at a time and hands it to this sink, which plays it on the
 * console's main port. sceAudioOutOutput returns once the port has taken
 * the grain, so the port clocks the driver. */
_Static_assert(PW_WINE_AUDIO_GRAIN == PW_AUDIO_PS5_GRAIN && PW_WINE_AUDIO_RATE == PW_AUDIO_PS5_RATE,
               "Wine's audio grain is the port's");
static PwAudioPs5Ops audio_ops;
static int audio_port = -1;

static int wine_audio(void *context, const int16_t *frames)
{
    (void)context;
    return audio_ops.output(audio_port, frames) < 0 ? -1 : 0;
}

/* Scale a frame onto the whole screen as the profile asks (fit keeps the
 * aspect ratio: 800x600 becomes 1440x1080), straight into the scanout, and
 * flip it. */
static int show_scaled(PwVideoOutPs5 *video, const PwPresentView *view, int scaling)
{
    const PwPresentFrame frame = { view->pixels, view->width, view->height, view->stride,
                                   PW_PRESENT_BGRX8 };

    return pw_videoout_ps5_present_scaled(video, &frame, scaling, 0x000000u);
}

/* ---- fd 1 and fd 2 -> ps5log ------------------------------------------- */

/* The in-process wineserver writes its errors with fprintf(stderr), and a
 * console program's output reaches Wine's standard output handle, fd 1,
 * neither of which a title shows: each becomes one end of a socket pair
 * (pipe() is not available and dup2 onto fd 2 is refused, measured) whose
 * lines a thread forwards as "WINESERVER ..." and "STDOUT ..." records. */
typedef struct PwFdForward { int reader; const char *tag; } PwFdForward;

static void *forward_fd(void *arg)
{
    const PwFdForward *forward = arg;
    char buffer[1024];
    size_t used = 0;

    for (;;) {
        ssize_t got = read(forward->reader, buffer + used, sizeof(buffer) - 1 - used);
        if (got <= 0) break;
        used += (size_t)got;
        for (;;) {
            char *newline = memchr(buffer, '\n', used);
            if (!newline && used < sizeof(buffer) - 1) break;
            size_t line = newline ? (size_t)(newline - buffer) : used;
            buffer[line] = 0;
            if (line && buffer[line - 1] == '\r') buffer[line - 1] = 0;
            PS5LOG_LOG("%s %s", forward->tag, buffer);
            line += newline ? 1u : 0u;
            memmove(buffer, buffer + line, used - line);
            used -= line;
        }
    }
    return NULL;
}

/* Close fd first: descriptors are allocated lowest first, so one end of the
 * new pair takes its number. Both ends of a socket pair are alike, so that
 * end is the writer and the other the reader, with no dup2. */
static int capture_fd(int fd, PwFdForward *forward)
{
    int pair[2];
    pthread_t thread;

    close(fd);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
    if (pair[0] == fd || pair[1] == fd) {
        forward->reader = pair[0] == fd ? pair[1] : pair[0];
    } else {
        /* lower numbers were free too: move the writer onto fd */
        forward->reader = pair[0];
        if (dup2(pair[1], fd) != fd) return -3;
    }
    return pthread_create(&thread, NULL, forward_fd, forward) ? -4 : 0;
}

/* ---- one game per process -------------------------------------------- */

static PwWineLaunch launch;

/* Replace this process with the title again: app's game, or the launcher
 * when app is NULL. /app0 is not visible once /data is granted, so the
 * sandbox's view of the same eboot is tried next. Returns only on failure. */
static void restart_title(const PwWineApp *app, uint32_t cycle, const char *reason)
{
    static const char *const eboots[] = { "/app0/eboot.bin", PW_SANDBOX_APP0 "/eboot.bin" };
    static char storage[2 * PW_WINE_LAUNCH_PATH_MAX + 64];
    char *next[PW_WINE_LAUNCH_ARGS];

    if (!pw_wine_launch_argv(app, cycle, storage, sizeof(storage), next, PW_WINE_LAUNCH_ARGS)) {
        PS5LOG_LOG("PW_WINE64 restart refused: arguments do not fit");
        return;
    }
    for (size_t i = 0; i < sizeof(eboots) / sizeof(eboots[0]); i++) {
        PS5LOG_LOG("PW_WINE64 restart to=%s cycle=%u reason=%s eboot=%s", app ? app->id : "launcher",
                   (unsigned)cycle, reason, eboots[i]);
        int rc = sceSystemServiceLoadExec(eboots[i], next);
        PS5LOG_LOG("PW_WINE64 restart failed rc=0x%08x", (unsigned)rc);
    }
}

/* Wine ends the process with exit() when its last program ends: go back to
 * the launcher. */
static void on_exit_report(void)
{
    PS5LOG_LOG("PW_WINE64 exit sink_calls=%lu", sink_calls);
    restart_title(NULL, launch.cycle + 1u, "wine-exit");
    ps5log_close("wine64-exit");
}

/* ---- library ------------------------------------------------------------ */

/* Request /data, as a game does, and read the games from it (or from the
 * sandbox's /download0 when /data does not appear). A process granted /data
 * cannot write the sandbox's /download0 (EACCES, measured), so the launcher
 * cannot read a copy there instead. */
static void open_library(void)
{
    PwDataMountResult mount;
    int status;

    if (pw_data_mount_request(&mount) == 0) library_root = PW_WINE64_ROOT_DATA;
    PS5LOG_LOG("PW_WINE64 data_mount data_before=%d prepare_errno=%d wrote=%d write_errno=%d data_after=%d "
               "waited_ms=%d settled_ms=%d root=%s", mount.data_before, mount.prepare_errno,
               mount.wrote_request, mount.write_errno, mount.data_after, mount.waited_ms,
               mount.settled_ms, library_root);
    status = pw_wine_library_load(&library, library_root);
    catalog_count = 0;
    for (uint32_t i = 0; i < library.count; i++) {
        const PwWineLibraryEntry *entry = &library.entries[i];
        if (entry->status != PW_OK) {
            PS5LOG_LOG("PW_WINE64 profile refused file=%s status=%s", entry->file,
                       pw_result_name(entry->status));
            continue;
        }
        const PwGameProfile *game = &entry->profile;
        if (game->display.width)
            snprintf(catalog_detail[catalog_count], sizeof(catalog_detail[0]), "%s  %s  %ux%u",
                     game->app.architecture == PW_APP_ARCH_PE64 ? "pe64" : "pe32",
                     game->app.graphics == PW_APP_GRAPHICS_DXVK ? "dxvk" : "gdi",
                     (unsigned)game->display.width, (unsigned)game->display.height);
        else
            snprintf(catalog_detail[catalog_count], sizeof(catalog_detail[0]), "%s  %s",
                     game->app.architecture == PW_APP_ARCH_PE64 ? "pe64" : "pe32",
                     game->app.graphics == PW_APP_GRAPHICS_DXVK ? "dxvk" : "gdi");
        catalog[catalog_count] = (PwWineApp){ game->app.id, game->app.name,
                                              catalog_detail[catalog_count], game->app.executable };
        catalog_count++;
    }
    PS5LOG_LOG("PW_WINE64 library status=%s listed_by=%d scan_errno=%d entries=%u games=%u "
               "root=%s", pw_result_name(status), library.listed_by, library.scan_error,
               (unsigned)library.count, (unsigned)catalog_count, library_root);
}

/* ---- launcher ----------------------------------------------------------- */

/* Shown until a game is chosen; Wine is not loaded. Does not return. */
static void run_launcher(void)
{
    static PwVideoOutPs5 video;
    static PwPadPs5 pad;
    PwPadPs5Ops pad_ops;
    PwLauncherItem items[PW_WINE_LIBRARY_MAX];
    const size_t frame_bytes = (size_t)PW_LAUNCHER_RENDER_WIDTH * PW_LAUNCHER_RENDER_HEIGHT * 4u;
    uint8_t *frame = mmap(NULL, frame_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    PwLauncherScene scene = { items, 0, PW_LAUNCHER_RENDER_NONE,
        launch.refused ? "THAT GAME IS NOT IN THE LIBRARY" :
        !catalog_count ? "ADD PROFILES TO /DATA/PROSPERO-WIN/PROFILES" :
        launch.cycle ? "WELCOME BACK" : "CHOOSE A GAME" };
    int video_status = pw_videoout_ps5_open(&video), pad_status = pw_pad_ps5_platform_ops(&pad_ops);
    int dirty = 1, chosen = -1;

    if (pad_status == PW_OK)
        pad_status = pw_pad_ps5_open(&pad, &pad_ops, pad_open_map,
                                     sizeof(pad_open_map) / sizeof(pad_open_map[0]));
    /* The games, then the refused profiles, listed by file as not available. */
    for (size_t i = 0; i < catalog_count; i++)
        items[scene.count++] = (PwLauncherItem){ catalog[i].name, catalog[i].detail, 1 };
    for (uint32_t i = 0; i < library.count; i++)
        if (library.entries[i].status != PW_OK)
            items[scene.count++] = (PwLauncherItem){ library.entries[i].file, "profile refused", 0 };
    if (scene.count) scene.selected = 0;
    PS5LOG_LOG("PW_WINE64 launcher video=%s pad=%s frame=%d apps=%u cycle=%u refused=%u script=%d",
               pw_result_name(video_status), pw_result_name(pad_status), frame != MAP_FAILED,
               (unsigned)catalog_count, (unsigned)launch.cycle, launch.refused, PW_WINE64_SCRIPT);
    for (uint64_t tick = 1; chosen < 0; tick++) {
        if (pad_status == PW_OK && pw_pad_ps5_read(&pad) == PW_OK) {
            uint32_t pressed = scene.count ? pad.core.pressed_edges : 0, before = scene.selected;
            if ((pressed & PAD_RIGHT) && scene.selected + 1 < scene.count) scene.selected++;
            if ((pressed & PAD_LEFT) && scene.selected) scene.selected--;
            if ((pressed & PAD_DOWN) && scene.selected + 3 < scene.count) scene.selected += 3;
            if ((pressed & PAD_UP) && scene.selected >= 3) scene.selected -= 3;
            if ((pressed & PAD_CROSS) && scene.selected < catalog_count) chosen = (int)scene.selected;
            dirty |= scene.selected != before;
        }
        if (PW_WINE64_SCRIPT && tick == 3 * PW_WINE64_TICKS_PER_S) {
            if (launch.cycle >= PW_WINE64_SCRIPT_CYCLES) {
                PS5LOG_LOG("PW_WINE64 launcher script done cycles=%u", (unsigned)launch.cycle);
                ps5log_close("wine64-script-done");
                _exit(0);
            }
            if (catalog_count) chosen = 0;
        }
        if (dirty && video_status == PW_OK && frame != MAP_FAILED) {
            const PwPresentTarget target = { frame, PW_LAUNCHER_RENDER_WIDTH, PW_LAUNCHER_RENDER_HEIGHT,
                                             PW_LAUNCHER_RENDER_WIDTH * 4u, frame_bytes };
            const PwPresentView view = { frame, target.width, target.height, target.stride,
                                           (uint32_t)frame_bytes };
            int status = pw_launcher_render(&scene, &target);
            if (status == PW_OK) status = pw_videoout_ps5_present(&video, &view);
            if (status != PW_OK) PS5LOG_LOG("PW_WINE64 launcher present=%s", pw_result_name(status));
            dirty = 0;
        } else {
            usleep(PW_WINE64_TICK_US);
        }
    }
    PS5LOG_LOG("PW_WINE64 launcher chose=%s", catalog[chosen].id);
    if (pad_status == PW_OK) (void)pw_pad_ps5_close(&pad);
    if (video_status == PW_OK) (void)pw_videoout_ps5_close(&video);
    restart_title(&catalog[chosen], launch.cycle, "launcher");
    ps5log_close("wine64-launch-failed");
    _exit(1);
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
    static char prefix[PW_WINE_LIBRARY_PATH + PW_APP_ID_CAPACITY], desktop[24], view[8] = "window";
    static PwWineStartEnv extra[] = {
        { "WINEDEBUG", PW_WINE64_DEBUG },
        /* the i386 exe runs in this process through WoW64; otherwise Wine
         * starts it from start.exe in a new process, which a title cannot */
        { "WINEARCH", "wow64" },
        { "HOME", prefix },
        { "USER", "prospero" },
        { "WINE_PS5_TRACE_STARTUP", "1" },  /* patch 0560: name startup steps */
        { "WINE_PS5_VIEW", view },          /* patch 0430: the game's windows, or the desktop */
        { "WINE_PS5_DESKTOP", desktop },    /* last: only when the profile sets it */
    };
    /* wine, the executable, the profile's argument words, NULL */
    static const char *wine_argv[2 + PW_WINE_LAUNCH_WORDS + 1] = { "wine" };
    static char argument_words[PW_APP_ARGUMENTS_CAPACITY];
    static char ntdll_dir[256], ntdll_path[288];
    static const PwWineStartOps ops = {
        sceKernelLoadStartModule, sceKernelGetModuleInfo, set_env, start_thread };
    static PwWineStartConfig config = {
        .ntdll_path = ntdll_path,
        .ntdll_dir = ntdll_dir,
        .prefix = prefix,
        .extra_env = extra, .extra_env_count = sizeof(extra) / sizeof(extra[0]) - 1,
        .argc = 2, .argv = wine_argv, .stack_bytes = 16u << 20,
    };
    static PwWineStart start;
    static PwVideoOutPs5 video;
    static PwPadPs5 pad;
    static PwGameInput game_input;
    const PwGameProfile *game = NULL;
    int scaling = PW_PRESENT_SCALE_FIT;
    PwWinePointer pointer = { 0, 0, 0, 0 };
    int (*post_input)(const PwWineInput *) = NULL;
    void (*set_pad)(const PwWinePad *) = NULL;
    int (*rumble)(uint32_t *, uint32_t *) = NULL;
    uint64_t vibrations = 0;
    uint64_t shown_sequence = 0, shown = 0, posted = 0, refused = 0;
    ps5log_config log_config;
    PwPadPs5Ops pad_ops;
    int status, video_status = PW_ERR_STATE, pad_status = PW_ERR_STATE;
    uint64_t close_requested = 0, combo_ticks = 0, started = now_ns();

    ps5log_config_defaults(&log_config);
    if (ps5log_load_config(ps5log_default_conf_paths, ps5log_default_conf_path_count,
                           &log_config, NULL) == 0)
        ps5log_init(&log_config, PW_TITLE_ID, PW_APP_NAME, now_ns());
    open_library();
    (void)pw_wine_launch_parse(argc, argv, catalog, catalog_count, &launch);
    PS5LOG_LOG("PW_WINE64 args argc=%d mode=%s profile=%s cycle=%u refused=%u", argc,
               launch.mode == PW_WINE_LAUNCH_GAME ? "game" : "launcher",
               launch.app ? launch.app->id : "-", (unsigned)launch.cycle, launch.refused);
    if (launch.mode != PW_WINE_LAUNCH_GAME) run_launcher();
    wine_argv[1] = launch.executable;
    /* The game's profile: its prefix, desktop, scaling and input. A bare
     * path= runs in the default prefix with nothing bound. */
    pw_game_input_init(&game_input);
    game = launch.app ? pw_wine_library_find(&library, launch.app->id) : NULL;
    if (game && strcmp(game->app.prefix, "default"))
        snprintf(prefix, sizeof(prefix), "%s/prefixes/%s", library_root, game->app.prefix);
    else
        snprintf(prefix, sizeof(prefix), "%s/prefix", library_root);
    if (game) {
        int input_status = pw_wine_library_input(game, library_root, &game_input);
        scaling = (int)game->display.scaling;
        if (game->display.view == PW_GAME_VIEW_DESKTOP) snprintf(view, sizeof(view), "desktop");
        if (game->display.width) {
            snprintf(desktop, sizeof(desktop), "%ux%u", (unsigned)game->display.width,
                     (unsigned)game->display.height);
            config.extra_env_count++;
        }
        PS5LOG_LOG("PW_WINE64 profile id=%s prefix=%s desktop=%s scaling=%d view=%s input=%s "
                   "preset=%s mode=%s mouse=%d", game->app.id, prefix, desktop[0] ? desktop : "default",
                   scaling, view, pw_result_name(input_status),
                   game->input.preset[0] ? game->input.preset : "-",
                   game_input.mode == PW_GAME_INPUT_XINPUT ? "xinput" : "keyboard", (int)game_input.mouse);
        /* [application] arguments follow the executable in Wine's argv. */
        int words = pw_wine_launch_split(game->app.arguments, argument_words,
                                         sizeof(argument_words), wine_argv + 2,
                                         PW_WINE_LAUNCH_WORDS);
        if (words > 0) config.argc = 2 + words;
        if (words < 0) PS5LOG_LOG("PW_WINE64 arguments refused: %s", game->app.arguments);
        else if (words) PS5LOG_LOG("PW_WINE64 arguments words=%d line=%s", words, game->app.arguments);
    }

    for (size_t i = 0; i < sizeof(runtime_roots) / sizeof(runtime_roots[0]); i++) {
        struct stat st;
        snprintf(ntdll_dir, sizeof(ntdll_dir), "%s" PW_WINE64_RUNTIME, runtime_roots[i]);
        snprintf(ntdll_path, sizeof(ntdll_path), "%s/ntdll.prx", ntdll_dir);
        if (stat(ntdll_path, &st) == 0) break;
        PS5LOG_LOG("PW_WINE64 runtime not at %s", ntdll_dir);
    }
    PS5LOG_LOG("PW_WINE64 ntdll=%s prefix=%s exe=%s", config.ntdll_path, config.prefix,
               launch.executable);
    {
        static PwFdForward forward_stderr = { -1, "WINESERVER" }, forward_stdout = { -1, "STDOUT" };
        status = capture_fd(2, &forward_stderr);
        if (status != 0) PS5LOG_LOG("PW_WINE64 fd2_capture=failed status=%d", status);
        status = capture_fd(1, &forward_stdout);
        if (status != 0) PS5LOG_LOG("PW_WINE64 fd1_capture=failed status=%d", status);
    }

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
        /* xinput mode: the DualSense is also the game's XInput controller
         * 0, read by Wine's xinput (patch 0470); bindings still apply. */
        if (game_input.mode == PW_GAME_INPUT_XINPUT) {
            set_pad = (void (*)(const PwWinePad *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_pad");
            rumble = (int (*)(uint32_t *, uint32_t *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_rumble");
        }
        video_status = pw_videoout_ps5_open(&video);
        pad_status = pw_pad_ps5_platform_ops(&pad_ops);
        if (pad_status == PW_OK)
            pad_status = pw_pad_ps5_open(&pad, &pad_ops, pad_open_map,
                                         sizeof(pad_open_map) / sizeof(pad_open_map[0]));
        {
            uint8_t *storage = mmap(NULL, 2u * PW_WINE64_MAX_FRAME, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANON, -1, 0);
            if (storage != MAP_FAILED &&
                pw_wine_frame_box_init(&frames, storage, PW_WINE64_MAX_FRAME) == 0) {
                frame_shown = storage + PW_WINE64_MAX_FRAME;
                if (set_present) set_present(wine_present, NULL);
            }
        }
        {
            void (*set_release)(PwWineDisplayRelease, void *) = (void (*)(PwWineDisplayRelease, void *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_display_release");
            if (set_release) set_release(wine_release_display, NULL);
        }
        PS5LOG_LOG("PW_WINE64 display present_sink=%d frames=%d post_input=%d xinput=%d video=%s "
                   "pad=%s", set_present != NULL, frame_shown != NULL, post_input != NULL,
                   set_pad != NULL,
                   pw_result_name(video_status),
                   pw_result_name(pad_status));
        {
            void (*set_audio)(PwWineAudioSink, void *) = (void (*)(PwWineAudioSink, void *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_wine_set_audio_sink");
            int audio_status = set_audio ? pw_audio_ps5_platform_ops(&audio_ops) : PW_ERR_NOT_FOUND;
            if (audio_status == PW_OK) audio_status = pw_audio_ps5_open_port(&audio_ops, &audio_port);
            if (audio_status == PW_OK) set_audio(wine_audio, NULL);
            PS5LOG_LOG("PW_WINE64 audio sink=%d port=%d status=%s", set_audio != NULL, audio_port,
                       pw_result_name(audio_status));
        }
        /* The game starts in its profile's working directory. A title
         * cannot chdir, so ntdll keeps a logical one (wine/ps5/pw_wine_cwd.h),
         * which Wine reads as the process's current directory. */
        if (game && game->app.working_directory[0]) {
            int (*set_cwd)(const char *) = (int (*)(const char *))
                (uintptr_t)pw_prx_lookup(start.descriptor, "pw_cwd_set");
            char host_dir[PW_WINE_LIBRARY_PATH + PW_APP_ID_CAPACITY + PW_APP_PATH_CAPACITY];
            int mapped = pw_wine_launch_host_dir(prefix, game->app.working_directory, host_dir,
                                                 sizeof(host_dir));
            int cwd_status = set_cwd && mapped == 0 ? set_cwd(host_dir) : -1;
            PS5LOG_LOG("PW_WINE64 cwd=%s host=%s status=%d", game->app.working_directory,
                       mapped == 0 ? host_dir : "-", cwd_status);
        }
        status = pw_wine_start_environment(&start, &config, &ops);
        PS5LOG_LOG("PW_WINE64 environment status=%d", status);
    }
    if (status == PW_OK) {
        status = pw_wine_start_run(&start, &config, &ops);
        PS5LOG_LOG("PW_WINE64 run status=%d", status);
    }
    for (uint64_t tick = 1; status == PW_OK; tick++) {
        uint64_t now = now_ns();
        PwPresentView view;
        PwWineInput events[2 * PW_GAME_BUTTON_COUNT + 1];
        int presented = 0;

        if (pad_status == PW_OK && post_input && pw_pad_ps5_read(&pad) == PW_OK) {
            size_t count = pw_wine_game_inputs(&game_input, pad.core.pressed_edges,
                                               pad.core.released_edges, events,
                                               sizeof(events) / sizeof(events[0]) - 1);
            /* A stick moves the pointer over what the frames show. It starts
             * in the middle and keeps its place when their size changes, as
             * when a menu widens the game's windows. */
            if (game_input.mouse != PW_GAME_STICK_NONE && frames.width) {
                if (!pointer.width)
                    pw_wine_pointer_init(&pointer, frames.width, frames.height);
                else if (pointer.width != frames.width || pointer.height != frames.height)
                    pw_wine_pointer_resize(&pointer, frames.width, frames.height);
                const PwPadPs5Stick *stick = game_input.mouse == PW_GAME_STICK_LEFT ?
                                             &pad.left_stick : &pad.right_stick;
                count += (size_t)pw_wine_pointer_step(&pointer, stick->x, stick->y,
                                                      game_input.mouse_speed,
                                                      PW_WINE64_TICK_US, &events[count]);
            }
            for (size_t i = 0; i < count; i++) {
                if (post_input(&events[i]) == 0) posted++;
                else refused++;
            }
            if (set_pad) {
                PwWinePad state;
                set_pad(pw_wine_game_pad(&pad, &state) ? &state : NULL);
            }
            /* The rumble the game asked for: XInput's left motor is the
             * DualSense's large one, speeds 0..65535 become 0..255. */
            uint32_t left, right;
            if (rumble && rumble(&left, &right) == 1) {
                int vibrate = pw_pad_ps5_vibrate(&pad, (uint8_t)(left >> 8), (uint8_t)(right >> 8));
                if (!vibrations++ || vibrate != PW_OK)
                    PS5LOG_LOG("PW_WINE64 rumble left=%u right=%u status=%s rc=%d", (unsigned)left,
                               (unsigned)right, pw_result_name(vibrate), pad.vibration_rc);
            }
        }
        if (__atomic_load_n(&display_request, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&display_closed, __ATOMIC_RELAXED)) {
            int closed = video_status == PW_OK ? pw_videoout_ps5_close(&video) : PW_OK;
            video_status = PW_ERR_STATE;
            __atomic_store_n(&display_closed, 1, __ATOMIC_RELEASE);
            PS5LOG_LOG("PW_WINE64 display released to vulkan close=%s shown=%llu",
                       pw_result_name(closed), (unsigned long long)shown);
        }
        if (video_status == PW_OK && frame_shown &&
            pw_wine_frame_box_take(&frames, &shown_sequence, frame_shown, PW_WINE64_MAX_FRAME,
                                   &view) == 1 &&
            show_scaled(&video, &view, scaling) == PW_OK) {
            shown++;          /* present waits for the vblank */
            presented = 1;
        }
        if (!presented) usleep(PW_WINE64_TICK_US);

        /* Closing: Options+Create held for a second, or the deadline, asks
         * the game to close with Alt+F4; Wine's exit then restarts the
         * title (on_exit_report). A game that does not close is left. */
        combo_ticks = pad_status == PW_OK && (pad.core.previous_buttons & PAD_CLOSE) == PAD_CLOSE ?
                      combo_ticks + 1 : 0;
        if (!close_requested &&
            (combo_ticks >= PW_WINE64_TICKS_PER_S ||
             (PW_WINE64_SECONDS && now - started >= (uint64_t)PW_WINE64_SECONDS * 1000000000u))) {
            static const PwWineInput alt_f4[] = {
                { PW_WINE_INPUT_KEY, 0x12, 0, 0, 1 }, { PW_WINE_INPUT_KEY, 0x73, 0, 0, 1 },
                { PW_WINE_INPUT_KEY, 0x73, 0, 0, 0 }, { PW_WINE_INPUT_KEY, 0x12, 0, 0, 0 },
            };
            close_requested = now;
            if (rumble && pad_status == PW_OK) (void)pw_pad_ps5_vibrate(&pad, 0, 0);
            for (size_t i = 0; post_input && i < sizeof(alt_f4) / sizeof(alt_f4[0]); i++)
                (void)post_input(&alt_f4[i]);
            PS5LOG_LOG("PW_WINE64 close requested by=%s", combo_ticks ? "combo" : "deadline");
        }
        if (close_requested && now - close_requested >= (uint64_t)PW_WINE64_CLOSE_WAIT_S * 1000000000u) {
            PS5LOG_LOG("PW_WINE64 close timeout: leaving the game");
            restart_title(NULL, launch.cycle + 1u, "close-timeout");
            break;
        }
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
            if (tick % 300 == 0) {
                uint64_t m[6] = { 0 };
                size_t flexible = 0;

                if (start.memory_stats) start.memory_stats(m, 6);
                (void)sceKernelAvailableFlexibleMemorySize(&flexible);
                PS5LOG_LOG("PW_WINE64 memory flexible_free=%zuK dmem=%lluK dmem_peak=%lluK runs=%llu "
                           "dmem_failures=%llu heap=%lluK heap_peak=%lluK",
                           flexible >> 10, (unsigned long long)(m[0] >> 10),
                           (unsigned long long)(m[1] >> 10), (unsigned long long)m[2],
                           (unsigned long long)m[3], (unsigned long long)(m[4] >> 10),
                           (unsigned long long)(m[5] >> 10));
            }
        }
    }
    PS5LOG_LOG("PW_WINE64 done status=%d stage=%d", status, start.stage);
    if (status != PW_OK) restart_title(NULL, launch.cycle + 1u, "start-failed");
    ps5log_close(status == PW_OK ? "wine64-restart-failed" : "wine64-start-failed");
    _exit(1);
}
