/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_osk_ps5.h"
#include "../include/prospero_win.h"
#include <assert.h>
#include <string.h>

/* The system's side of the dialog, as the adapter sees it. */
static struct {
    int loaded, initialized, terminated, inits;
    int status, outcome, init_rc, result_rc;
    const PwOskPs5Param *param;
    uint16_t typed[PW_OSK_PS5_TEXT];
} system_;

static int fake_init(const PwOskPs5Param *param, const void *extended)
{
    assert(!extended);                  /* the published call takes none */
    system_.param = param;
    system_.inits++;
    if (system_.init_rc) return system_.init_rc;
    /* The dialog writes what was typed into the title's buffer. */
    memcpy(param->input_text_buffer, system_.typed,
           sizeof(uint16_t) * (param->max_text_length + 1));
    return 0;
}

static int fake_status(void) { return system_.status; }

static int fake_result(PwOskPs5Result *result)
{
    result->outcome = system_.outcome;
    return system_.result_rc;
}

static int fake_term(void) { system_.terminated++; return 0; }

static int fake_common_dialog(void) { system_.initialized++; return 0; }

static int32_t fake_load(const char *name)
{
    if (strcmp(name, "libSceImeDialog")) return -1;
    system_.loaded++;
    return 7;
}

static int fake_resolve(int32_t module, const char *name, void **address)
{
    assert(module == 7);
    if (!strcmp(name, "sceImeDialogInit")) *address = (void *)(uintptr_t)fake_init;
    else if (!strcmp(name, "sceImeDialogGetStatus")) *address = (void *)(uintptr_t)fake_status;
    else if (!strcmp(name, "sceImeDialogGetResult")) *address = (void *)(uintptr_t)fake_result;
    else if (!strcmp(name, "sceImeDialogTerm")) *address = (void *)(uintptr_t)fake_term;
    else return -1;
    return 0;
}

static char logged[256];
static void fake_log(const char *line)
{
    size_t length = strlen(line);
    if (length >= sizeof(logged)) length = sizeof(logged) - 1;
    memcpy(logged, line, length);
    logged[length] = '\0';
}

static const PwOskPs5Ops ops = { fake_load, fake_resolve, fake_common_dialog, fake_log };

/* What the dialog will report as typed, as the system stores it. */
static void system_text(const char *utf8)
{
    memset(system_.typed, 0, sizeof(system_.typed));
    pw_osk_ps5_utf16(utf8, system_.typed, PW_OSK_PS5_TEXT);
}

static void test_conversion(void)
{
    uint16_t utf16[16];
    char utf8[32];

    assert(pw_osk_ps5_utf16("hi!", utf16, 16) == 3);
    assert(utf16[0] == 'h' && utf16[1] == 'i' && utf16[2] == '!' && !utf16[3]);
    assert(pw_osk_ps5_utf8(utf16, utf8, sizeof(utf8)) == 3 && !strcmp(utf8, "hi!"));

    /* Two bytes in, two bytes out. */
    assert(pw_osk_ps5_utf16("a\xc3\xb1", utf16, 16) == 2 && utf16[1] == 0xf1);
    assert(pw_osk_ps5_utf8(utf16, utf8, sizeof(utf8)) == 3 && !strcmp(utf8, "a\xc3\xb1"));

    /* Outside the basic plane: a surrogate pair each way. */
    assert(pw_osk_ps5_utf16("\xf0\x9f\x8e\xae", utf16, 16) == 2);
    assert(utf16[0] == 0xd83c && utf16[1] == 0xdfae);
    assert(pw_osk_ps5_utf8(utf16, utf8, sizeof(utf8)) == 4 && !strcmp(utf8, "\xf0\x9f\x8e\xae"));

    /* Malformed input becomes U+FFFD rather than reaching the system. */
    assert(pw_osk_ps5_utf16("\xc3", utf16, 16) == 1 && utf16[0] == 0xfffd);
    assert(pw_osk_ps5_utf16("\x80z", utf16, 16) == 2 && utf16[0] == 0xfffd && utf16[1] == 'z');
    /* A lone surrogate from the system does not become one in UTF-8. */
    utf16[0] = 0xd800; utf16[1] = 'x'; utf16[2] = 0;
    assert(pw_osk_ps5_utf8(utf16, utf8, sizeof(utf8)) == 4 && utf8[3] == 'x');

    /* Both stop where the room does, and still terminate. */
    assert(pw_osk_ps5_utf16("abcd", utf16, 3) == 2 && !utf16[2]);
    assert(pw_osk_ps5_utf8(utf16, utf8, 2) == 1 && !strcmp(utf8, "a"));
    assert(!pw_osk_ps5_utf16("a", NULL, 4) && !pw_osk_ps5_utf8(utf16, utf8, 0));
    assert(!pw_osk_ps5_utf16(NULL, utf16, 4) && !utf16[0]);
}

static void test_dialog(void)
{
    PwOskPs5 osk;
    char text[PW_OSK_TEXT];

    memset(&system_, 0, sizeof(system_));
    assert(pw_osk_ps5_open(&osk, &ops, 42) == PW_OK);
    assert(system_.loaded == 1 && system_.initialized == 1 && osk.ready && osk.module == 7);
    assert(!strcmp(logged, "keyboard module=0x7 ready=1 common=0 user=42"));
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == PW_ERR_STATE);  /* nothing is up */

    system_text("prospero");
    assert(pw_osk_ps5_show(&osk, "Half-Life 2") == PW_OK && osk.showing && osk.shown == 1);
    assert(pw_osk_ps5_show(&osk, "again") == PW_ERR_STATE && system_.inits == 1);
    /* The system was given the title's own buffers and the user's id. */
    assert(system_.param == &osk.param);
    assert(system_.param->user_id == 42 && system_.param->type == 1);
    assert(system_.param->input_text_buffer == osk.text);
    assert(system_.param->max_text_length == PW_OSK_PS5_TEXT - 1);
    assert(system_.param->title == osk.title && osk.title[0] == 'H');
    assert(!system_.param->filter && !system_.param->placeholder && !system_.param->option);

    system_.status = PW_OSK_PS5_RUNNING;
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 0 && !text[0] && !system_.terminated);
    system_.status = PW_OSK_PS5_FINISHED;
    system_.outcome = PW_OSK_PS5_OK;
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 1);
    assert(!strcmp(text, "prospero") && osk.accepted == 1 && system_.terminated == 1);
    assert(!osk.showing && pw_osk_ps5_poll(&osk, text, sizeof(text)) == PW_ERR_STATE);

    /* Cancelled: the dialog is ended and nothing is typed. */
    system_text("ignored");
    assert(pw_osk_ps5_show(&osk, NULL) == PW_OK);
    system_.outcome = PW_OSK_PS5_CANCELLED;
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 1 && !text[0]);
    assert(osk.cancelled == 1 && system_.terminated == 2 && osk.accepted == 1);

    /* A result the system refuses to give counts as aborted. */
    assert(pw_osk_ps5_show(&osk, NULL) == PW_OK);
    system_.result_rc = -1;
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 1 && !text[0]);
    assert(osk.last_outcome == PW_OSK_PS5_ABORTED && system_.terminated == 3);
    system_.result_rc = 0;

    /* A dialog that never comes up, or goes away on its own, gives the pad
     * back: a status of NONE ends it once the grace has passed. */
    assert(pw_osk_ps5_show(&osk, NULL) == PW_OK);
    system_.status = PW_OSK_PS5_NONE;
    for (unsigned i = 1; i < PW_OSK_PS5_GRACE_POLLS; i++)
        assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 0 && osk.showing);
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == PW_ERR_STATE);
    assert(!osk.showing && system_.terminated == 4);

    /* One that comes up a few polls late is still the user's. */
    assert(pw_osk_ps5_show(&osk, NULL) == PW_OK);
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 0);
    system_.status = PW_OSK_PS5_RUNNING;
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 0 && osk.showing);
    system_.status = PW_OSK_PS5_FINISHED;
    system_.outcome = PW_OSK_PS5_OK;
    system_text("late");
    memcpy(osk.text, system_.typed, sizeof(osk.text));
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == 1 && !strcmp(text, "late"));
    assert(system_.terminated == 5);

    /* A dialog the system refuses to open leaves nothing to poll. */
    system_.status = PW_OSK_PS5_FINISHED;
    system_.init_rc = (int)0x80b80002;
    assert(pw_osk_ps5_show(&osk, NULL) == PW_ERR_STATE && !osk.showing && osk.shown == 5);
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == PW_ERR_STATE);
}

/* Without the module there is no keyboard, and the title is told so. */
static void test_unavailable(void)
{
    PwOskPs5 osk;
    char text[8];
    PwOskPs5Ops broken = ops;

    memset(&system_, 0, sizeof(system_));
    broken.load_module = NULL;
    assert(pw_osk_ps5_open(&osk, &broken, 1) == PW_ERR_PRECONDITION);
    assert(pw_osk_ps5_open(NULL, &ops, 1) == PW_ERR_PRECONDITION);

    broken = ops;
    broken.resolve = NULL;
    assert(pw_osk_ps5_open(&osk, &broken, 1) == PW_ERR_PRECONDITION);

    broken = ops;
    broken.common_dialog_init = NULL;   /* no way to reach the dialog family */
    assert(pw_osk_ps5_open(&osk, &broken, 1) == PW_OK);
    assert(osk.common_dialog_rc == PW_ERR_NOT_FOUND);
    assert(pw_osk_ps5_show(&osk, "x") == PW_OK);   /* it is still tried */

    memset(&osk, 0, sizeof(osk));
    assert(pw_osk_ps5_show(&osk, "x") == PW_ERR_STATE);
    assert(pw_osk_ps5_poll(&osk, text, sizeof(text)) == PW_ERR_STATE);
    assert(pw_osk_ps5_poll(&osk, NULL, sizeof(text)) == PW_ERR_PRECONDITION);
    assert(pw_osk_ps5_show(NULL, "x") == PW_ERR_PRECONDITION);
}

int main(void)
{
    test_conversion();
    test_dialog();
    test_unavailable();
    return 0;
}
