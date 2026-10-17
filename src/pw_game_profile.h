/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_GAME_PROFILE_H
#define PW_GAME_PROFILE_H
/*
 * A game's profile for the Wine title: the [application] section read by
 * pw_app_profile, plus how the game is shown and played.
 *
 *   [display]
 *   desktop = 800x600      ; Wine's desktop size (default: the driver's)
 *   scaling = fit          ; fit (keep aspect, fill the screen), integer, stretch
 *   view = window          ; window: show the game's windows; desktop: all of it
 *   show_fps = true        ; a frame-rate counter in the top left (default: true):
 *                          ; DXVK's HUD, or Mesa's for graphics = opengl
 *   refresh = 120          ; accepted and ignored: it set the PS5 OpenGL SDK's
 *   opengl_thread = true   ; output rate and glthread, which Zink replaced
 *
 *   [input]
 *   preset = pinball       ; an input file shared between profiles
 *   mode = keyboard        ; keyboard (buttons send keys/mouse) or xinput (also
 *                          ; the game's XInput controller); default: xinput
 *                          ; when nothing is bound, keyboard otherwise
 *   mouse = right_stick    ; a stick moves the pointer: left_stick, right_stick, none
 *   mouse_speed = 1200     ; pointer pixels per second at full tilt
 *   cross = space          ; <button> = <key> | mouse_left | mouse_right |
 *   r2 = mouse_left        ;            mouse_middle | vk:0xNN | none
 *   player2 = pinball-p2   ; a keyboard preset for a second DualSense, the pad
 *                          ; of another signed-in user (local multiplayer)
 *
 *   [debug]
 *   winedebug = +seh,+virtual   ; Wine's debug channels for this game (default:
 *                               ; the title's, err+all,+loaddll,+process)
 *   env = PW_NATIVE_PROFILE=1   ; one more variable for Wine's environment,
 *                               ; up to 5 env lines: names start with PW_,
 *                               ; DXVK_, MESA_, GALLIUM_, ZINK_, RADV_ or VK_,
 *                               ; and may not be one the title sets itself
 *                               ; (PW_VK_BATCH, PW_INPUT_SHARED_FAST,
 *                               ; PW_QPC_TSC_*, GALLIUM_DRIVER, and DXVK_HUD
 *                               ; or GALLIUM_HUD while show_fps is on);
 *                               ; values: letters, digits and _ + - , . = : /
 *
 *   [runtime]
 *   thread_scheduling = true    ; Windows threads take turns and get their
 *                               ; priorities (WINE_PS5_SCHED=1); true/false
 *                               ; or 1/0, default false
 *   cpu = native                ; a 32-bit game's CPU backend: native (runs
 *                               ; its code directly, with Vulkan batching) or
 *                               ; translator (wowprospero); default native,
 *                               ; translator for builtin OpenGL games; native
 *                               ; for explicit graphics=zink
 *   shared_input = true         ; GetKeyState answered from Wine's shared
 *                               ; input memory, without a crossing
 *                               ; (PW_INPUT_SHARED_FAST=1); default false
 *   fast_clock = true           ; QueryPerformanceCounter from the TSC
 *                               ; (patch 0900): the title measures the TSC
 *                               ; frequency at launch and sets
 *                               ; PW_QPC_TSC_VALIDATED/PW_QPC_TSC_HZ only
 *                               ; when it is consistent; default false
 *
 * Buttons: cross circle square triangle l1 r1 l2 r2 l3 r3 up down left
 * right options create touchpad. An input preset file holds only an [input]
 * section; the profile's own [input] lines override it. Unknown sections,
 * keys and values are refused, so a typo never plays with defaults.
 */
#include "pw_app_profile.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_GAME_BUTTON_COUNT = 17,
    PW_GAME_DESKTOP_MIN_W = 320, PW_GAME_DESKTOP_MIN_H = 200,
    PW_GAME_DESKTOP_MAX_W = 3840, PW_GAME_DESKTOP_MAX_H = 2160,
    PW_GAME_MOUSE_SPEED_DEFAULT = 1200, PW_GAME_MOUSE_SPEED_MAX = 20000,
    PW_GAME_WINEDEBUG_CAPACITY = 128,
    PW_GAME_DEBUG_ENV_MAX = 5, PW_GAME_DEBUG_ENV_NAME = 48, PW_GAME_DEBUG_ENV_VALUE = 96,
};

typedef enum PwGameScaling { PW_GAME_SCALING_FIT = 0, PW_GAME_SCALING_INTEGER,
                             PW_GAME_SCALING_STRETCH } PwGameScaling;
typedef enum PwGameView { PW_GAME_VIEW_WINDOW = 0, PW_GAME_VIEW_DESKTOP } PwGameView;
typedef enum PwGameInputMode { PW_GAME_INPUT_KEYBOARD = 0, PW_GAME_INPUT_XINPUT } PwGameInputMode;
typedef enum PwGameStick { PW_GAME_STICK_NONE = 0, PW_GAME_STICK_LEFT, PW_GAME_STICK_RIGHT } PwGameStick;
typedef enum PwGameBindKind { PW_GAME_BIND_UNSET = 0, PW_GAME_BIND_NONE, PW_GAME_BIND_KEY,
                              PW_GAME_BIND_MOUSE } PwGameBindKind;

typedef struct PwGameBinding {
    uint32_t mask;      /* the DualSense button (scePadRead bit) */
    uint8_t kind;       /* PwGameBindKind */
    uint16_t code;      /* KEY: Windows virtual key; MOUSE: 0 left, 1 right, 2 middle */
} PwGameBinding;

typedef struct PwGameInput {
    PwGameInputMode mode;
    PwGameStick mouse;
    uint32_t mouse_speed;
    PwGameBinding bindings[PW_GAME_BUTTON_COUNT];   /* in PW_GAME_BUTTON order */
    char preset[PW_APP_ID_CAPACITY];
    char player2[PW_APP_ID_CAPACITY];   /* the second pad's preset, profile only */
    uint32_t set;       /* which of mode/mouse/mouse_speed a file set (overlay) */
} PwGameInput;

typedef struct PwGameDisplay {
    uint32_t width, height;     /* 0x0: the driver's default */
    PwGameScaling scaling;
    PwGameView view;            /* window by default: a small game fills the TV */
    int show_fps;               /* the graphics backend's frame-rate counter */
    uint32_t refresh;           /* parsed (60 or 120) but unused since Zink */
    int opengl_thread;          /* parsed but unused since Zink */
} PwGameDisplay;

/* [runtime] cpu: the CPU backend a 32-bit game runs under. */
typedef enum PwGameCpu {
    PW_GAME_CPU_DEFAULT = 0,    /* native */
    PW_GAME_CPU_NATIVE = 1,
    PW_GAME_CPU_TRANSLATOR = 2,
} PwGameCpu;

typedef struct PwGameRuntime {
    int thread_scheduling;      /* WINE_PS5_SCHED=1 */
    PwGameCpu cpu;
    int shared_input;           /* PW_INPUT_SHARED_FAST=1 */
    int fast_clock;             /* the title calibrates the TSC, then sets
                                   PW_QPC_TSC_VALIDATED and PW_QPC_TSC_HZ */
} PwGameRuntime;

/* A variable of Wine's environment; both strings are static. */
typedef struct PwGameEnv { const char *name, *value; } PwGameEnv;

/* [debug] env = NAME=VALUE: one extra variable for Wine's environment. */
typedef struct PwGameDebugEnv {
    char name[PW_GAME_DEBUG_ENV_NAME];
    char value[PW_GAME_DEBUG_ENV_VALUE];
} PwGameDebugEnv;
enum { PW_GAME_RUNTIME_ENV_MAX = 2, PW_GAME_CPU_ENV_MAX = 2, PW_GAME_GRAPHICS_ENV_MAX = 3,
       PW_GAME_CLOCK_ENV_MAX = 2 };

typedef struct PwGameProfile {
    PwAppProfile app;
    PwGameDisplay display;
    PwGameInput input;          /* this profile's own [input] lines */
    /* [debug] winedebug: WINEDEBUG for this game, empty for the title's. */
    char winedebug[PW_GAME_WINEDEBUG_CAPACITY];
    PwGameDebugEnv debug_env[PW_GAME_DEBUG_ENV_MAX];    /* [debug] env lines */
    size_t debug_env_count;
    PwGameRuntime runtime;      /* [runtime], everything off by default */
} PwGameProfile;

/* Parse a whole profile. [application] must come first; [display],
 * [input], [debug] and [runtime] are optional, once each. PW_OK or a
 * PW_ERR_* status. */
int pw_game_profile_parse(const uint8_t *bytes, size_t length, PwGameProfile *profile);
/* The variables runtime sets in Wine's environment, into env, which has
 * room for PW_GAME_RUNTIME_ENV_MAX; how many. fast_clock is not among them:
 * its frequency is measured at launch (src/pw_tsc_calibrate.h), and the
 * title adds up to PW_GAME_CLOCK_ENV_MAX variables itself. */
size_t pw_game_runtime_env(const PwGameRuntime *runtime, PwGameEnv *env);
/* Whether a game runs on the native WoW64 CPU: only 32-bit games; [runtime]
 * cpu when set, else native. OpenGL games draw through Zink, whose Vulkan
 * calls the batching covers like DXVK's. */
int pw_game_cpu_native(const PwGameProfile *profile);
/* The variables that select the native CPU, which always comes with Vulkan
 * batching (WINE_PS5_WOW64_CPU, PW_VK_BATCH), into env, which has room for
 * PW_GAME_CPU_ENV_MAX; how many (0 for the translator). */
size_t pw_game_cpu_env(const PwGameProfile *profile, PwGameEnv *env);

/* Graphics-specific environment, at most PW_GAME_GRAPHICS_ENV_MAX entries.
 * graphics = opengl (or zink) selects GALLIUM_DRIVER=zink and
 * PW_VK_DEFER_DESCRIPTORS=1 (unless [debug] env sets that variable, the
 * opt-out), with Mesa's FPS HUD when show_fps is on; the launcher installs the architecture's Zink
 * provider from win/mesa-zink/{i386-windows,x86_64-windows} and refuses the
 * launch when it is missing. */
size_t pw_game_graphics_env(const PwGameProfile *profile, PwGameEnv *env);
/* The [debug] env variables, into env, which has room for
 * PW_GAME_DEBUG_ENV_MAX; how many. The strings live in profile. */
size_t pw_game_debug_env(const PwGameProfile *profile, PwGameEnv *env);
/* An input with nothing bound, keyboard mode, no pointer. */
void pw_game_input_init(PwGameInput *input);
/* Parse an input preset file (one [input] section, no preset= line) into
 * input, overriding what it sets. PW_OK or a PW_ERR_* status. */
int pw_game_input_parse(const uint8_t *bytes, size_t length, PwGameInput *input);
/* Apply what overrides sets on top of base (a preset under a profile). */
void pw_game_input_overlay(PwGameInput *base, const PwGameInput *overrides);
/* After the overlays: an input that sets no mode, binds no button and moves
 * no pointer is in xinput mode, so a game with gamepad support reads the
 * DualSense while the keyboard and mouse keep working. */
void pw_game_input_default_mode(PwGameInput *input);
/* The DualSense bit of button index i (PW_GAME_BUTTON order), 0 if none. */
uint32_t pw_game_button_mask(size_t index);
#endif
