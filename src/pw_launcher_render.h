/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_LAUNCHER_RENDER_H
#define PW_LAUNCHER_RENDER_H
#include "pw_present.h"

/* Software-drawn launcher screen for the fixed 1920x1080 output.  The look
 * is inspired by a classic blue-and-green desktop theme; every shape, colour
 * ramp and the 5x7 glyph set are original to this file.  The scene holds
 * plain strings, so the renderer knows nothing about profiles or Win32. */
enum {
    PW_LAUNCHER_RENDER_WIDTH=1920,PW_LAUNCHER_RENDER_HEIGHT=1080,
    PW_LAUNCHER_RENDER_PAGE=6,   /* tiles visible at once (3 x 2) */
    PW_LAUNCHER_RENDER_NONE=UINT32_MAX
};
typedef struct PwLauncherItem {
    const char *title;           /* shown upper-cased; unknown glyphs as '?' */
    const char *detail;          /* optional second line */
    unsigned available;          /* 0: listed as not available yet */
} PwLauncherItem;
typedef struct PwLauncherScene {
    const PwLauncherItem *items;
    uint32_t count,selected;     /* selected==PW_LAUNCHER_RENDER_NONE: no highlight */
    const char *status;          /* optional taskbar status text */
} PwLauncherScene;

/* Draws the whole screen into target (exactly 1920x1080, BGRX, any valid
 * stride).  The page shown is the one containing the selection. */
int pw_launcher_render(const PwLauncherScene *scene,const PwPresentTarget *target);

#endif
