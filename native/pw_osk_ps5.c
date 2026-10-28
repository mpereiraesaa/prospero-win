/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_osk_ps5.h"
#include "../include/prospero_win.h"
#include <stdio.h>
#include <string.h>

/* The layout the system reads; a mismatch would be read as garbage
 * pointers, so it is checked where it is written. */
_Static_assert(sizeof(PwOskPs5Param) == 96, "unexpected IME dialog parameter layout");
_Static_assert(sizeof(PwOskPs5Result) == 16, "unexpected IME dialog result layout");

enum {
    SCE_IME_TYPE_BASIC_LATIN = 1,
    SCE_IME_ALIGN_CENTER = 1,
    SCE_COMMON_DIALOG_ALREADY_INITIALIZED = (int)0x80b80002,
};

size_t pw_osk_ps5_utf16(const char *utf8, uint16_t *out, size_t capacity)
{
    const unsigned char *text = (const unsigned char *)utf8;
    size_t written = 0;

    if (!out || !capacity) return 0;
    while (text && *text && written + 1 < capacity) {
        uint32_t code = *text;
        unsigned length = 1, i;

        if ((code & 0xe0u) == 0xc0u) { code &= 0x1fu; length = 2; }
        else if ((code & 0xf0u) == 0xe0u) { code &= 0x0fu; length = 3; }
        else if ((code & 0xf8u) == 0xf0u) { code &= 0x07u; length = 4; }
        else if (code & 0x80u) { code = 0xfffdu; length = 1; }
        for (i = 1; i < length; i++) {
            if ((text[i] & 0xc0u) != 0x80u) { code = 0xfffdu; length = 1; break; }
            code = (code << 6) | (text[i] & 0x3fu);
        }
        if (code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu)) code = 0xfffdu;
        text += length;
        if (code >= 0x10000u) {
            if (written + 2 >= capacity) break;
            code -= 0x10000u;
            out[written++] = (uint16_t)(0xd800u | (code >> 10));
            out[written++] = (uint16_t)(0xdc00u | (code & 0x3ffu));
        } else {
            out[written++] = (uint16_t)code;
        }
    }
    out[written] = 0;
    return written;
}

size_t pw_osk_ps5_utf8(const uint16_t *utf16, char *out, size_t capacity)
{
    size_t written = 0;

    if (!out || !capacity) return 0;
    for (size_t i = 0; utf16 && utf16[i]; i++) {
        uint32_t code = utf16[i];
        unsigned length;

        if (code >= 0xd800u && code <= 0xdbffu && utf16[i + 1] >= 0xdc00u &&
            utf16[i + 1] <= 0xdfffu)
            code = 0x10000u + ((code - 0xd800u) << 10) + (utf16[++i] - 0xdc00u);
        else if (code >= 0xd800u && code <= 0xdfffu)
            code = 0xfffdu;
        length = code < 0x80u ? 1u : code < 0x800u ? 2u : code < 0x10000u ? 3u : 4u;
        if (written + length >= capacity) break;
        switch (length) {
        case 1: out[written++] = (char)code; break;
        case 2:
            out[written++] = (char)(0xc0u | (code >> 6));
            out[written++] = (char)(0x80u | (code & 0x3fu));
            break;
        case 3:
            out[written++] = (char)(0xe0u | (code >> 12));
            out[written++] = (char)(0x80u | ((code >> 6) & 0x3fu));
            out[written++] = (char)(0x80u | (code & 0x3fu));
            break;
        default:
            out[written++] = (char)(0xf0u | (code >> 18));
            out[written++] = (char)(0x80u | ((code >> 12) & 0x3fu));
            out[written++] = (char)(0x80u | ((code >> 6) & 0x3fu));
            out[written++] = (char)(0x80u | (code & 0x3fu));
            break;
        }
    }
    out[written] = '\0';
    return written;
}

static void logf_(const PwOskPs5 *osk, const char *line)
{
    if (osk->ops.log) osk->ops.log(line);
}

int pw_osk_ps5_open(PwOskPs5 *osk, const PwOskPs5Ops *ops, int32_t user_id)
{
    char line[192];
    int32_t module;

    if (!osk || !ops || !ops->load_module || !ops->resolve) return PW_ERR_PRECONDITION;
    memset(osk, 0, sizeof(*osk));
    osk->ops = *ops;
    osk->user_id = user_id;
    osk->module = module = ops->load_module("libSceImeDialog");
    osk->ready = module >= 0 &&
        !ops->resolve(module, "sceImeDialogInit", (void **)&osk->dialog_init) &&
        !ops->resolve(module, "sceImeDialogGetStatus", (void **)&osk->dialog_status) &&
        !ops->resolve(module, "sceImeDialogGetResult", (void **)&osk->dialog_result) &&
        !ops->resolve(module, "sceImeDialogTerm", (void **)&osk->dialog_term);
    osk->common_dialog_rc = PW_ERR_STATE;   /* brought up with the first dialog */
    snprintf(line, sizeof(line), "keyboard module=%#x ready=%d user=%d",
             (unsigned)module, osk->ready, (int)user_id);
    logf_(osk, line);
    return osk->ready ? PW_OK : PW_ERR_NOT_FOUND;
}

int pw_osk_ps5_show(PwOskPs5 *osk, const char *title)
{
    char line[160];

    if (!osk) return PW_ERR_PRECONDITION;
    if (!osk->ready) return PW_ERR_STATE;
    if (osk->showing) return PW_ERR_STATE;
    if (!osk->common_tried) {
        /* The dialog family is brought up once; a second caller is told it
         * already is, which is the state this needs. */
        osk->common_tried = 1;
        osk->common_dialog_rc = osk->ops.common_dialog_init ? osk->ops.common_dialog_init() :
                                PW_ERR_NOT_FOUND;
        snprintf(line, sizeof(line), "keyboard common=%#x", (unsigned)osk->common_dialog_rc);
        logf_(osk, line);
    }
    memset(osk->text, 0, sizeof(osk->text));
    pw_osk_ps5_utf16(title ? title : "prospero-win", osk->title, PW_OSK_PS5_TITLE);
    memset(&osk->param, 0, sizeof(osk->param));
    osk->param.user_id = osk->user_id;
    osk->param.type = SCE_IME_TYPE_BASIC_LATIN;   /* the keys a game can be sent */
    osk->param.max_text_length = PW_OSK_PS5_TEXT - 1;
    osk->param.input_text_buffer = osk->text;
    osk->param.horizontal_alignment = SCE_IME_ALIGN_CENTER;
    osk->param.vertical_alignment = SCE_IME_ALIGN_CENTER;
    osk->param.title = osk->title;
    osk->last_init_rc = osk->dialog_init(&osk->param, NULL);
    osk->showing = osk->last_init_rc == 0;
    osk->polls = 0;
    if (osk->showing) osk->shown++;
    snprintf(line, sizeof(line), "keyboard shown=%d init=%#x", osk->showing,
             (unsigned)osk->last_init_rc);
    logf_(osk, line);
    return osk->showing ? PW_OK : PW_ERR_STATE;
}

/* Ends the dialog whatever happened to it, so the next one can open. */
static int finish(PwOskPs5 *osk, int result)
{
    osk->showing = 0;
    (void)osk->dialog_term();
    return result;
}

void pw_osk_ps5_close(PwOskPs5 *osk)
{
    if (!osk || !osk->showing) return;
    logf_(osk, "keyboard ended by=title");
    (void)finish(osk, 0);
}

int pw_osk_ps5_poll(PwOskPs5 *osk, char *text, size_t capacity)
{
    PwOskPs5Result result;
    char line[160];
    int status, rc;

    if (!osk || !text || !capacity) return PW_ERR_PRECONDITION;
    text[0] = '\0';
    if (!osk->showing) return PW_ERR_STATE;
    status = osk->dialog_status();
    osk->polls++;
    if (status == PW_OSK_PS5_RUNNING) return 0;
    if (status != PW_OSK_PS5_FINISHED) {
        /* NONE: the dialog is not there. Early on it may not have been
         * drawn yet; past the grace it is gone, and nothing was typed. */
        if (osk->polls < PW_OSK_PS5_GRACE_POLLS) return 0;
        snprintf(line, sizeof(line), "keyboard ended status=none polls=%u", osk->polls);
        logf_(osk, line);
        return finish(osk, PW_ERR_STATE);
    }
    memset(&result, 0, sizeof(result));
    rc = osk->dialog_result(&result);
    osk->last_outcome = rc == 0 ? result.outcome : PW_OSK_PS5_ABORTED;
    if (rc == 0 && result.outcome == PW_OSK_PS5_OK) {
        pw_osk_ps5_utf8(osk->text, text, capacity);
        osk->accepted++;
    } else {
        osk->cancelled++;
    }
    snprintf(line, sizeof(line), "keyboard ended result=%#x outcome=%d characters=%u",
             (unsigned)rc, osk->last_outcome, (unsigned)strlen(text));
    logf_(osk, line);
    return finish(osk, 1);
}

#ifndef PW_OSK_PS5_HOST_TEST
int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t *handle);
int sceKernelDlsym(int32_t handle, const char *symbol, void **address);
int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv,
                                 uint32_t flags, const void *option, int *result);
int sceKernelAvailableFlexibleMemorySize(size_t *size);

static int32_t sysmodule_handle(uint16_t id)
{
    int32_t handle = -1;

    if (sceSysmoduleGetModuleHandleInternal(id, &handle) == 0 && handle >= 0) return handle;
    handle = -1;
    if (sceSysmoduleGetModuleHandleInternal(0x80000000u | id, &handle) == 0 && handle >= 0) return handle;
    return -1;
}

static int32_t load(const char *name)
{
    int status;
    int32_t handle;

    if (strcmp(name, "libSceImeDialog")) return -1;
    status = sceSysmoduleLoadModule(PW_OSK_PS5_MODULE);
    handle = sysmodule_handle(PW_OSK_PS5_MODULE);
    return handle >= 0 ? handle : status < 0 ? status : -1;
}

/* sceCommonDialogInitialize, which libSceCommonDialog exports. It has no
 * sysmodule id: it comes in behind libSceImeDialog, so it is looked for on
 * that module first and then as a module of its own, by the names a title
 * may ask for. PW_ERR_NOT_FOUND when no way reached it. */
static int common_dialog_init(void)
{
    static const char *const paths[] = { "libSceCommonDialog.sprx",
                                         "/system/common/lib/libSceCommonDialog.sprx" };
    int (*initialize)(void) = NULL;
    int32_t handle = sysmodule_handle(PW_OSK_PS5_MODULE);

    if (handle >= 0 && sceKernelDlsym(handle, "sceCommonDialogInitialize",
                                      (void **)&initialize) == 0 && initialize)
        return initialize();
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int result = 0;
        int32_t module = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &result);

        if (module >= 0 && sceKernelDlsym(module, "sceCommonDialogInitialize",
                                          (void **)&initialize) == 0 && initialize)
            return initialize();
    }
    return PW_ERR_NOT_FOUND;
}

void pw_osk_ps5_preload(void (*log)(const char *line))
{
    char line[160];
    size_t before = 0, after = 0;
    int status;

    (void)sceKernelAvailableFlexibleMemorySize(&before);
    status = sceSysmoduleLoadModule(PW_OSK_PS5_MODULE);
    (void)sceKernelAvailableFlexibleMemorySize(&after);
    snprintf(line, sizeof(line), "preload libSceImeDialog id=%#x status=%#x handle=%d "
             "flexible_free=%zuK->%zuK", (unsigned)PW_OSK_PS5_MODULE, (unsigned)status,
             (int)sysmodule_handle(PW_OSK_PS5_MODULE), before >> 10, after >> 10);
    if (log) log(line);
}

const PwOskPs5Ops *pw_osk_ps5_system_ops(void (*log)(const char *line))
{
    static PwOskPs5Ops ops;
    ops.load_module = load;
    ops.resolve = sceKernelDlsym;
    ops.common_dialog_init = common_dialog_init;
    ops.log = log;
    return &ops;
}
#endif
