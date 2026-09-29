/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PROSPERO_WIN_PW_APP_PROFILE_H
#define PROSPERO_WIN_PW_APP_PROFILE_H

#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_APP_PROFILE_MAX_BYTES = 8192,
    PW_APP_ID_CAPACITY = 65,
    PW_APP_NAME_CAPACITY = 97,
    PW_APP_PATH_CAPACITY = 260,
    PW_APP_ARGUMENTS_CAPACITY = 513,
    PW_APP_DLL_OVERRIDES_CAPACITY = 257,
    PW_APP_RUNTIME_CAPACITY = 65,
};

typedef enum PwAppArchitecture {
    PW_APP_ARCH_PE32 = 1,
    PW_APP_ARCH_PE64 = 2,
} PwAppArchitecture;

typedef enum PwAppGraphics {
    PW_APP_GRAPHICS_AUTO = 0,
    PW_APP_GRAPHICS_GDI = 1,
    PW_APP_GRAPHICS_DXVK = 2,
    PW_APP_GRAPHICS_OPENGL = 3,
} PwAppGraphics;

/* Stable, allocation-free description of one Windows application. Paths are
 * absolute DOS paths; prefix and runtime are validated identifiers, not paths.
 * This describes a launch request and does not imply that its architecture or
 * graphics mode is supported by the currently selected runtime. */
typedef struct PwAppProfile {
    char id[PW_APP_ID_CAPACITY];
    char name[PW_APP_NAME_CAPACITY];
    char executable[PW_APP_PATH_CAPACITY];
    char working_directory[PW_APP_PATH_CAPACITY];
    char arguments[PW_APP_ARGUMENTS_CAPACITY];
    /* Optional: Wine's DLL load order for this game, in WINEDLLOVERRIDES
     * syntax (d3d11,dxgi=n uses the game's own DXVK DLLs); empty if unset. */
    char dll_overrides[PW_APP_DLL_OVERRIDES_CAPACITY];
    /* Optional initial WM_COMMAND queued when the first window enters wait. */
    uint32_t startup_command_id;
    char prefix[PW_APP_ID_CAPACITY];
    char runtime[PW_APP_RUNTIME_CAPACITY];
    PwAppArchitecture architecture;
    PwAppGraphics graphics;
} PwAppProfile;

/* Parse the bounded [application] INI format documented in WINE_INTEGRATION.md.
 * The output is written only on success. Unknown, duplicate and missing fields
 * are rejected so profile typos cannot silently launch with defaults. */
int pw_app_profile_parse(const uint8_t *bytes, size_t length,
                         PwAppProfile *profile);

/* Return the per-game Wine overrides selected by this graphics mode. OpenGL
 * profiles force Wine's builtin opengl32 so WGL reaches the PS5 EGL backend. */
int pw_app_profile_effective_dll_overrides(const PwAppProfile *profile,
                                           char *text, size_t capacity);

#endif
