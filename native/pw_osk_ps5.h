/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_OSK_PS5_H
#define PW_OSK_PS5_H
/*
 * The console's on-screen keyboard, the system's IME dialog, as the title
 * opens it over a running game (src/pw_osk.h is the portable half).
 *
 * The dialog lives in libSceImeDialog, which a title may not load by path
 * (0x80020063, as for libSceKeyboard: native/pw_hid_ps5.h): the system
 * loads it as its module, SCE_SYSMODULE_IME_DIALOG = 0x0096, and its
 * functions are then looked up. Like the keyboard's modules it is loaded
 * before the title asks for /data, since a process whose credentials
 * changed is reported to fail to load system modules; PS5 homebrew that
 * opens this dialog (ps5-payload-dev's offact, EVO-PLAYER-PS5,
 * ProsperoTV) loads it the same way and reports the same ordering. A
 * firmware that refuses the module to a title refuses it the way it
 * refuses the keyboard's, and says so in the preload's log line: this
 * adapter then reports no keyboard and the game is played as before.
 *
 * Text is held as uint16_t rather than wchar_t on purpose: the character
 * the console reads is 16 bits wide whatever the toolchain's wchar_t is.
 *
 * The dialog family also wants sceCommonDialogInitialize once per process
 * (0x80B80002 ALREADY_INITIALIZED counts as initialised). It lives in
 * libSceCommonDialog, which has no sysmodule id of its own and comes in
 * behind libSceImeDialog; each way of reaching it is tried and logged, and
 * the dialog is opened even when none worked, so a console run says which
 * one the firmware wanted rather than refusing in advance.
 *
 * The flow is the published one: sceImeDialogInit, then sceImeDialogGetStatus
 * until it reports FINISHED, then sceImeDialogGetResult and sceImeDialogTerm.
 * The system draws the dialog over the title's video output and takes the
 * DualSense while it is up, so the title keeps presenting the game's frames
 * and sees the pad intercepted.
 *
 * Text is UTF-16LE in a buffer the caller owns and the dialog writes; this
 * adapter converts to and from UTF-8 at the boundary. The parameter block
 * is the 96-byte one PS4 and PS5 reimplementations agree on (shadPS4's
 * OrbisImeDialogParam, OpenOrbis's OrbisImeDialogSetting, and the PS5
 * projects above, which assert its size).
 */
#include "../src/pw_osk.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_OSK_PS5_MODULE = 0x0096,         /* SCE_SYSMODULE_IME_DIALOG */
    PW_OSK_PS5_TEXT = PW_OSK_TEXT,      /* characters, with the terminator */
    PW_OSK_PS5_TITLE = 64,
    /* sceImeDialogGetStatus */
    PW_OSK_PS5_NONE = 0, PW_OSK_PS5_RUNNING = 1, PW_OSK_PS5_FINISHED = 2,
    /* SceImeDialogResult's outcome */
    PW_OSK_PS5_OK = 0, PW_OSK_PS5_CANCELLED = 1, PW_OSK_PS5_ABORTED = 2,
    /* Polls a dialog that says NONE is given before it is called gone, so
     * a system that reports it only once it is drawn still gets there.
     * The title polls once a frame, so this is about a second. */
    PW_OSK_PS5_GRACE_POLLS = 60,
};

typedef struct PwOskPs5Ops {
    int32_t (*load_module)(const char *name);                 /* "libSceImeDialog": handle, or < 0 */
    int (*resolve)(int32_t module, const char *name, void **address);
    /* sceCommonDialogInitialize, or NULL when it could not be reached. */
    int (*common_dialog_init)(void);
    void (*log)(const char *line);
} PwOskPs5Ops;

/* The dialog's parameters, exactly as the system reads them. */
typedef struct PwOskPs5Param {
    int32_t user_id;
    int32_t type;
    uint64_t supported_languages;
    int32_t enter_label;
    int32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_text_length;
    uint16_t *input_text_buffer;
    float posx, posy;
    int32_t horizontal_alignment;
    int32_t vertical_alignment;
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} PwOskPs5Param;

typedef struct PwOskPs5Result {
    int32_t outcome;
    int8_t reserved[12];
} PwOskPs5Result;

typedef struct PwOskPs5 {
    PwOskPs5Ops ops;
    int32_t user_id, module;
    int ready;                  /* the symbols resolved */
    int common_dialog_rc;       /* what sceCommonDialogInitialize said */
    int showing;
    int (*dialog_init)(const PwOskPs5Param *param, const void *extended);
    int (*dialog_status)(void);
    int (*dialog_result)(PwOskPs5Result *result);
    int (*dialog_term)(void);
    int last_init_rc, last_outcome;
    unsigned polls;             /* of the dialog now up */
    unsigned long shown, accepted, cancelled;
    /* The dialog writes here while it is up, so it outlives the call. */
    PwOskPs5Param param;
    uint16_t text[PW_OSK_PS5_TEXT];
    uint16_t title[PW_OSK_PS5_TITLE];
} PwOskPs5;

/* Looks the dialog's functions up and brings the common-dialog family up;
 * logs each step. PW_OK when a dialog can be opened. */
int pw_osk_ps5_open(PwOskPs5 *osk, const PwOskPs5Ops *ops, int32_t user_id);

/* Shows the keyboard with title above it, starting empty. PW_OK when the
 * system took it; the title then polls. */
int pw_osk_ps5_show(PwOskPs5 *osk, const char *title);

/* One poll. 0 while the dialog is up; 1 once it closed, with the accepted
 * line in text as UTF-8 (empty when it was cancelled or aborted); a
 * negative pw_result when it could not be read, which also ends it. A
 * dialog that never comes up ends this way after PW_OSK_PS5_GRACE_POLLS,
 * so a refused one gives the pad back instead of keeping it. */
int pw_osk_ps5_poll(PwOskPs5 *osk, char *text, size_t capacity);

/* Text conversion at the boundary, for tests: both NUL-terminate and
 * return how many units they wrote, dropping what does not fit. */
size_t pw_osk_ps5_utf16(const char *utf8, uint16_t *out, size_t capacity);
size_t pw_osk_ps5_utf8(const uint16_t *utf16, char *out, size_t capacity);

/* The system's functions, on the console. */
const PwOskPs5Ops *pw_osk_ps5_system_ops(void (*log)(const char *line));
/* Loads the dialog's module early, before the title's /data grant. */
void pw_osk_ps5_preload(void (*log)(const char *line));
#endif
