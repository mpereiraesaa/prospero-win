/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_osk.h"
#include <assert.h>
#include <string.h>

enum { OPTIONS = PW_OSK_OPTIONS, TOUCHPAD = PW_OSK_TOUCHPAD, COMBO = PW_OSK_COMBO,
       SQUARE = 0x8000u, CROSS = 0x4000u };

static PwOsk osk;
static PwOskView view;
static uint32_t held;

/* One tick from the previous held set to `now`, edges derived as the pad
 * core does. */
static int tick(uint32_t now)
{
    uint32_t pressed = now & ~held, released = held & ~now;
    held = now;
    return pw_osk_tick(&osk, now, pressed, released, &view);
}

static void assert_view(uint32_t h, uint32_t p, uint32_t r, int neutral)
{
    assert(view.held == h && view.pressed == p && view.released == r && view.neutral == neutral);
}

/* The keys of one tick's typing. */
static size_t keys(PwOskKey *out)
{
    return pw_osk_keys(&osk, out, PW_OSK_KEYS_PER_CHARACTER);
}

static void assert_key(const PwOskKey *key, uint16_t vk, uint8_t down)
{
    assert(key->vk == vk && key->down == down);
}

/* Types a whole line a tick at a time and collects its key events. */
static size_t type_all(PwOskKey *out, size_t capacity)
{
    size_t count = 0;

    for (int guard = 0; guard < 4096; guard++) {
        PwOskKey batch[PW_OSK_KEYS_PER_CHARACTER];
        size_t got = keys(batch);

        if (!got) break;
        assert(count + got <= capacity);
        memcpy(out + count, batch, got * sizeof(batch[0]));
        count += got;
    }
    return count;
}

static void test_characters(void)
{
    int shift = -1;

    assert(pw_osk_character_vk('a', &shift) == 0x41 && !shift);
    assert(pw_osk_character_vk('z', &shift) == 0x5a && !shift);
    assert(pw_osk_character_vk('A', &shift) == 0x41 && shift);
    assert(pw_osk_character_vk('1', &shift) == 0x31 && !shift);
    assert(pw_osk_character_vk('!', &shift) == 0x31 && shift);
    assert(pw_osk_character_vk(')', &shift) == 0x30 && shift);
    assert(pw_osk_character_vk(' ', &shift) == 0x20 && !shift);
    assert(pw_osk_character_vk('\t', &shift) == 0x09 && !shift);
    assert(pw_osk_character_vk('\n', &shift) == 0x0d && !shift);
    assert(pw_osk_character_vk('\r', &shift) == 0x0d && !shift);
    assert(pw_osk_character_vk(';', &shift) == 0xba && !shift);
    assert(pw_osk_character_vk(':', &shift) == 0xba && shift);
    assert(pw_osk_character_vk('-', &shift) == 0xbd && !shift);
    assert(pw_osk_character_vk('_', &shift) == 0xbd && shift);
    assert(pw_osk_character_vk('`', &shift) == 0xc0 && !shift);
    assert(pw_osk_character_vk('~', &shift) == 0xc0 && shift);
    assert(pw_osk_character_vk('\\', &shift) == 0xdc && !shift);
    assert(pw_osk_character_vk('|', &shift) == 0xdc && shift);
    assert(pw_osk_character_vk('\'', &shift) == 0xde && !shift);
    assert(pw_osk_character_vk('"', &shift) == 0xde && shift);
    /* A character no US key types, and a byte of a UTF-8 sequence. */
    assert(!pw_osk_character_vk('\x01', &shift) && !shift);
    assert(!pw_osk_character_vk((char)0xc3, &shift) && !shift);
    assert(!pw_osk_character_vk('\0', &shift) && !shift);
    assert(pw_osk_character_vk('a', NULL) == 0x41);
}

/* The combo opens the keyboard, the line is typed, Enter closes it. */
static void test_combo_and_typing(void)
{
    PwOskKey typed[64];
    size_t count;

    memset(&osk, 0, sizeof(osk));
    held = 0;
    /* Closed: the game gets the pad as it is. */
    assert(!tick(SQUARE));
    assert_view(SQUARE, SQUARE, 0, 0);
    assert(!pw_osk_request(&osk));
    assert(!tick(SQUARE | OPTIONS));
    assert_view(SQUARE | OPTIONS, OPTIONS, 0, 0);   /* Options alone is the game's */

    /* Options + the touchpad click: the keyboard takes the pad and the
     * title is asked to show the dialog. */
    assert(tick(SQUARE | COMBO));
    assert_view(0, 0, SQUARE | OPTIONS, 1);
    assert(pw_osk_request(&osk) && osk.state == PW_OSK_OPENING);
    pw_osk_opened(&osk, 1);
    assert(!pw_osk_request(&osk) && osk.state == PW_OSK_SHOWN && osk.opened == 1);

    /* The dialog is the system's: nothing reaches the game while it is up,
     * and nothing is typed yet. */
    assert(tick(0));
    assert_view(0, 0, 0, 1);
    assert(!keys(typed));

    pw_osk_finished(&osk, "Hi!");
    assert(osk.state == PW_OSK_TYPING && osk.typed == 1);
    /* The pad is the game's again, except the buttons still held. */
    assert(!tick(TOUCHPAD));
    assert_view(0, 0, 0, 0);
    assert(!tick(TOUCHPAD | CROSS));
    assert_view(CROSS, CROSS, 0, 0);

    count = type_all(typed, sizeof(typed) / sizeof(typed[0]));
    assert(count == 4 + 2 + 4 + 2);
    assert_key(&typed[0], 0x10, 1);     /* Shift for H */
    assert_key(&typed[1], 'H', 1);
    assert_key(&typed[2], 'H', 0);
    assert_key(&typed[3], 0x10, 0);
    assert_key(&typed[4], 'I', 1);      /* i, unshifted */
    assert_key(&typed[5], 'I', 0);
    assert_key(&typed[6], 0x10, 1);     /* Shift for ! */
    assert_key(&typed[7], 0x31, 1);
    assert_key(&typed[8], 0x31, 0);
    assert_key(&typed[9], 0x10, 0);
    assert_key(&typed[10], 0x0d, 1);    /* the line is sent */
    assert_key(&typed[11], 0x0d, 0);
    assert(osk.state == PW_OSK_CLOSED && !keys(typed));

    /* The combo opens it again only once the buttons were let go. */
    assert(!tick(TOUCHPAD | CROSS) && osk.state == PW_OSK_CLOSED);
    assert(!tick(0));
    assert(tick(COMBO) && pw_osk_request(&osk));
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, NULL);        /* cancelled */
    assert(osk.state == PW_OSK_CLOSED && osk.cancelled == 1 && !keys(typed));
    assert(!tick(COMBO));               /* held, not pressed again */
    assert(!tick(0));
}

/* One character a tick, and only as many keys as the caller takes. */
static void test_paced(void)
{
    PwOskKey typed[8];

    memset(&osk, 0, sizeof(osk));
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "ab");
    assert(keys(typed) == 2 && typed[0].vk == 'A');
    assert(keys(typed) == 2 && typed[0].vk == 'B');
    assert(keys(typed) == 2 && typed[0].vk == 0x0d);
    assert(!keys(typed) && osk.state == PW_OSK_CLOSED);

    /* A caller with room for fewer keys than the character needs waits. */
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "A");
    assert(!pw_osk_keys(&osk, typed, 3) && osk.state == PW_OSK_TYPING);
    assert(pw_osk_keys(&osk, typed, 4) == 4 && typed[1].vk == 'A');
    assert(!pw_osk_keys(&osk, typed, 1));          /* Enter needs two */
    assert(pw_osk_keys(&osk, typed, 2) == 2 && typed[0].vk == 0x0d);
}

/* Characters the keyboard cannot type are dropped, the line is not. */
static void test_unmappable(void)
{
    PwOskKey typed[16];
    size_t count;

    memset(&osk, 0, sizeof(osk));
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "a\xc3\xb1z");           /* "añz" in UTF-8 */
    count = type_all(typed, sizeof(typed) / sizeof(typed[0]));
    assert(count == 6);
    assert_key(&typed[0], 'A', 1);
    assert_key(&typed[2], 'Z', 1);
    assert_key(&typed[4], 0x0d, 1);
}

/* A line longer than the buffer is typed as far as it fits. */
static void test_long_line(void)
{
    char line[PW_OSK_TEXT + 32];
    PwOskKey typed[2 * PW_OSK_TEXT + 4];
    size_t count;

    memset(line, 'a', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    memset(&osk, 0, sizeof(osk));
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, line);
    count = type_all(typed, sizeof(typed) / sizeof(typed[0]));
    assert(count == 2 * (PW_OSK_TEXT - 1) + 2);
    assert(osk.state == PW_OSK_CLOSED);
}

/* A dialog that does not come up gives the pad straight back. */
static void test_refused(void)
{
    PwOskKey typed[4];

    memset(&osk, 0, sizeof(osk));
    held = 0;
    assert(tick(COMBO) && pw_osk_request(&osk));
    pw_osk_opened(&osk, 0);
    assert(osk.state == PW_OSK_CLOSED && !osk.opened);
    assert(!tick(COMBO));
    assert_view(0, 0, 0, 0);            /* the combo stays hidden from the game */
    pw_osk_finished(&osk, "ignored");   /* no dialog was shown */
    assert(osk.state == PW_OSK_CLOSED && !keys(typed));
}

static void test_bad_arguments(void)
{
    PwOskKey typed[4];
    PwOskView ignored;

    memset(&osk, 0, sizeof(osk));
    assert(!pw_osk_tick(NULL, 0, 0, 0, &ignored));
    assert(!pw_osk_tick(&osk, 0, 0, 0, NULL));
    assert(!pw_osk_request(NULL));
    pw_osk_open(NULL);
    pw_osk_opened(NULL, 1);
    pw_osk_finished(NULL, "x");
    assert(!pw_osk_keys(NULL, typed, sizeof(typed) / sizeof(typed[0])));
    assert(!pw_osk_keys(&osk, NULL, 4));
}

int main(void)
{
    test_characters();
    test_combo_and_typing();
    test_paced();
    test_unmappable();
    test_long_line();
    test_refused();
    test_bad_arguments();
    return 0;
}
