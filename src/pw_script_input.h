/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Recorded input that an unattended run (the title's script build) replays
 * into a game: keys and the mouse through the path a USB keyboard and mouse
 * take, and a controller through the game's XInput controller 0 (games
 * whose profile has [input] mode = xinput). One event per line of
 * <root>/pw_script_input, its time in milliseconds after the replay starts.
 *
 *   sync <path under the library root> <text>   (optional, first)
 *   <ms> key <virtual-key code> <1 down | 0 up>
 *   <ms> button <0 left | 1 right | 2 middle> <1 down | 0 up>
 *   <ms> move <dx> <dy>                          (relative, desktop pixels)
 *   <ms> pad <buttons> <1 down | 0 up>
 *   <ms> stick <l | r> <x> <y>                   (-32768..32767, y up)
 *
 * pad buttons are an XInput name (a b x y start back lb rb ls rs up down
 * left right guide) or an XINPUT_GAMEPAD_* mask of them (0x1000 is A). A
 * pad button stays down, and a stick where it was put, until a later event
 * changes it; "stick l 0 0" lets go of the left stick. The scripted
 * controller is added to the real one: its buttons join the held ones, and
 * a stick it holds off centre takes the place of the real stick.
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
    PW_SCRIPT_INPUT_PAD = 4,
    PW_SCRIPT_INPUT_STICK = 5,
} PwScriptInputKind;

/* XINPUT_GAMEPAD_* buttons a pad event may name. */
enum { PW_SCRIPT_PAD_BUTTONS = 0xf7ffu };

typedef struct PwScriptInputEvent {
    uint32_t at_ms;
    uint32_t kind;  /* PwScriptInputKind */
    uint32_t code;  /* KEY: virtual key; BUTTON: 0 left, 1 right, 2 middle;
                       PAD: XInput button mask; STICK: 0 left, 1 right */
    int32_t dx, dy; /* MOVE; STICK: x, y */
    uint32_t down;  /* KEY, BUTTON, PAD */
} PwScriptInputEvent;

typedef struct PwScriptInput {
    PwScriptInputEvent *events; /* the caller's storage */
    size_t count;
    size_t pad_events; /* PAD and STICK events: the macro drives a controller */
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

/* The scripted controller: what the macro's pad and stick events hold. */
typedef struct PwScriptPad {
    uint16_t buttons;     /* XINPUT_GAMEPAD_* held */
    int16_t stick[2][2];  /* [0 left | 1 right][0 x | 1 y], y up; 0 0 lets go */
} PwScriptPad;

/* Applies one replayed event to the scripted controller. 1 when it was a
 * PAD or STICK event and now holds, 0 for any other kind (left to the
 * caller). *tick_changed collects the buttons changed since the caller
 * cleared it, once per frame: -1, and nothing applied, for a pad event
 * that would change one of them again (a release in the frame of its
 * press), which the caller replays next frame, so every press reaches the
 * game for at least one frame. */
int pw_script_pad_apply(PwScriptPad *pad, const PwScriptInputEvent *event, uint16_t *tick_changed);
