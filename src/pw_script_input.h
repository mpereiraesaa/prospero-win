/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Recorded input that an unattended run (the title's script build) replays
 * into a game through the path a USB keyboard and mouse take: one event per
 * line of <root>/pw_script_input, its time in milliseconds after the replay
 * starts.
 *
 *   sync <path under the library root> <text>   (optional, first)
 *   <ms> key <virtual-key code> <1 down | 0 up>
 *   <ms> button <0 left | 1 right | 2 middle> <1 down | 0 up>
 *   <ms> move <dx> <dy>                          (relative, desktop pixels)
 *
 * Without a sync line the replay starts with the game; with one, when the
 * text first appears in what that file gains after the game starts (a
 * game's own log once its level loaded).
 * Blank lines and lines starting with # are skipped; times never go back. */
#pragma once

#include "../include/prospero_win.h"
#include <stddef.h>
#include <stdint.h>

typedef enum PwScriptInputKind {
    PW_SCRIPT_INPUT_KEY = 1,
    PW_SCRIPT_INPUT_BUTTON = 2,
    PW_SCRIPT_INPUT_MOVE = 3,
} PwScriptInputKind;

typedef struct PwScriptInputEvent {
    uint32_t at_ms;
    uint32_t kind;  /* PwScriptInputKind */
    uint32_t code;  /* KEY: virtual key; BUTTON: 0 left, 1 right, 2 middle */
    int32_t dx, dy; /* MOVE */
    uint32_t down;  /* KEY, BUTTON */
} PwScriptInputEvent;

typedef struct PwScriptInput {
    PwScriptInputEvent *events; /* the caller's storage */
    size_t count;
    char sync_path[256]; /* empty: start with the game */
    char sync_text[128];
} PwScriptInput;

/* Lines of text, an upper bound on its events: size the storage with it. */
size_t pw_script_input_lines(const char *text, size_t length);

/* Parses text (length bytes, not necessarily NUL-terminated) into out,
 * whose events go to the caller's storage of capacity entries. PW_OK,
 * PW_ERR_MALFORMED with *bad_line set to the 1-based line refused,
 * PW_ERR_LIMIT past capacity. */
int pw_script_input_parse(const char *text, size_t length, PwScriptInputEvent *storage, size_t capacity,
                          PwScriptInput *out, size_t *bad_line);

/* Searches a growing file for the sync text without rereading it: feed each
 * new chunk in order; returns 1 once the text has appeared, across chunk
 * boundaries too. */
typedef struct PwScriptInputSync {
    const char *text;
    size_t matched; /* bytes of text matched at the end of what was fed */
} PwScriptInputSync;
int pw_script_input_sync_feed(PwScriptInputSync *sync, const char *chunk, size_t length);
