/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_osk.h"
#include <assert.h>
#include <string.h>

enum { OPTIONS = 0x8u, TOUCHPAD = PW_OSK_TOUCHPAD, SQUARE = 0x8000u, CROSS = 0x4000u };

static PwOsk osk;
static PwOskView view;
static uint32_t held;

/* One tick from the previous held set to `now`, edges derived as the pad
 * core does; two: the touchpad click that began this tick had two fingers. */
static int tick2(uint32_t now, int two)
{
    uint32_t pressed = now & ~held, released = held & ~now;
    held = now;
    return pw_osk_tick(&osk, now, pressed, released, two, &view);
}
static int tick(uint32_t now) { return tick2(now, 0); }

static void assert_view(uint32_t h, uint32_t p, uint32_t r, int neutral)
{
    assert(view.held == h && view.pressed == p && view.released == r && view.neutral == neutral);
}

/* The keys of one tick's typing. */
static size_t keys(PwOskKey *out)
{
    return pw_osk_keys(&osk, out, PW_OSK_KEYS_PER_TICK);
}

static void assert_key(const PwOskKey *key, uint16_t vk, uint8_t down)
{
    assert(key->vk == vk && key->down == down);
}

/* Types a whole line a tick at a time and collects its key events; *ticks
 * is how many ticks it took. */
static size_t type_all(PwOskKey *out, size_t capacity, unsigned *ticks)
{
    size_t count = 0;
    unsigned taken = 0;

    for (int guard = 0; guard < 4096; guard++) {
        PwOskKey batch[PW_OSK_KEYS_PER_TICK];
        size_t got = keys(batch);

        if (!got) break;
        taken++;
        assert(count + got <= capacity);
        memcpy(out + count, batch, got * sizeof(batch[0]));
        count += got;
    }
    if (ticks) *ticks = taken;
    return count;
}

/* Every key goes down in one tick and up in a later one, never both in
 * the same tick, and nothing is left down. */
static void assert_held_a_tick(void)
{
    PwOskKey batch[PW_OSK_KEYS_PER_TICK];
    int down[256] = { 0 };
    size_t got;

    while ((got = keys(batch)) != 0) {
        int pressed[256] = { 0 };
        for (size_t i = 0; i < got; i++) {
            uint8_t vk = (uint8_t)batch[i].vk;
            if (batch[i].down) { assert(!down[vk]); down[vk] = pressed[vk] = 1; }
            else { assert(down[vk] && !pressed[vk]); down[vk] = 0; }
        }
    }
    for (int vk = 0; vk < 256; vk++) assert(!down[vk]);
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

/* A two-finger click opens the keyboard, the line is typed, Enter ends it. */
static void test_gesture_and_typing(void)
{
    PwOskKey typed[64];
    size_t count;
    unsigned ticks;

    memset(&osk, 0, sizeof(osk));
    held = 0;
    /* Closed: the game gets the pad as it is, a one-finger click and
     * Options included, in the tick they happen. */
    assert(!tick(SQUARE));
    assert_view(SQUARE, SQUARE, 0, 0);
    assert(!tick(SQUARE | OPTIONS));
    assert_view(SQUARE | OPTIONS, OPTIONS, 0, 0);
    assert(!tick(SQUARE | OPTIONS | TOUCHPAD));
    assert_view(SQUARE | OPTIONS | TOUCHPAD, TOUCHPAD, 0, 0);
    assert(!pw_osk_request(&osk));
    assert(!tick(0));
    assert_view(0, 0, SQUARE | OPTIONS | TOUCHPAD, 0);

    /* The two-finger click: the keyboard takes the pad in that same tick,
     * the game never sees the click, and the title is asked for the dialog. */
    assert(!tick(SQUARE));
    assert(tick2(SQUARE | TOUCHPAD, 1));
    assert_view(0, 0, SQUARE, 1);
    assert(pw_osk_request(&osk) && osk.state == PW_OSK_OPENING);
    pw_osk_opened(&osk, 1);
    assert(!pw_osk_request(&osk) && osk.state == PW_OSK_SHOWN && osk.opened == 1);

    /* The dialog is the system's: nothing reaches the game while it is up,
     * and nothing is typed yet. */
    assert(tick(0));
    assert_view(0, 0, 0, 1);
    assert(!keys(typed));

    assert(tick(TOUCHPAD));             /* the system's click, not the game's */
    pw_osk_finished(&osk, "Hi!");
    assert(osk.state == PW_OSK_TYPING && osk.typed == 1);
    /* The pad is the game's again, except the buttons still held. */
    assert(!tick(TOUCHPAD));
    assert_view(0, 0, 0, 0);
    assert(!tick(TOUCHPAD | CROSS));
    assert_view(CROSS, CROSS, 0, 0);

    count = type_all(typed, sizeof(typed) / sizeof(typed[0]), &ticks);
    assert(count == 12 && ticks == 5);
    assert_key(&typed[0], 0x10, 1);     /* tick 1: Shift and H down */
    assert_key(&typed[1], 'H', 1);
    assert_key(&typed[2], 'H', 0);      /* tick 2: H and Shift up, I down */
    assert_key(&typed[3], 0x10, 0);
    assert_key(&typed[4], 'I', 1);
    assert_key(&typed[5], 'I', 0);      /* tick 3: I up, Shift and 1 down for ! */
    assert_key(&typed[6], 0x10, 1);
    assert_key(&typed[7], 0x31, 1);
    assert_key(&typed[8], 0x31, 0);     /* tick 4: up, and Enter down */
    assert_key(&typed[9], 0x10, 0);
    assert_key(&typed[10], 0x0d, 1);
    assert_key(&typed[11], 0x0d, 0);    /* tick 5: Enter up */
    assert(osk.state == PW_OSK_CLOSED && !keys(typed));

    /* A two-finger click while typing is still not the game's. */
    memset(&osk, 0, sizeof(osk));
    held = 0;
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "x");
    assert(!tick2(TOUCHPAD, 1) && osk.state == PW_OSK_TYPING);
    assert_view(0, 0, 0, 0);
    assert(!tick(TOUCHPAD));
    assert_view(0, 0, 0, 0);
    assert(!tick(0));
    assert_view(0, 0, 0, 0);
    assert(type_all(typed, sizeof(typed) / sizeof(typed[0]), NULL) == 4);

    /* Cancelled: nothing is typed, and the click that opened it stays
     * hidden until it is let go. */
    assert(tick2(TOUCHPAD, 1) && pw_osk_request(&osk));
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, NULL);
    assert(osk.state == PW_OSK_CLOSED && osk.cancelled == 1 && !keys(typed));
    assert(!tick(TOUCHPAD));
    assert_view(0, 0, 0, 0);
    assert(!tick(0));
    assert_view(0, 0, 0, 0);
}

/* A key a tick, held for a tick; a key typed twice is let go in between;
 * a caller without room for a whole tick's keys gets none. */
static void test_paced(void)
{
    PwOskKey typed[8];

    memset(&osk, 0, sizeof(osk));
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "ab");
    assert(keys(typed) == 1);
    assert_key(&typed[0], 'A', 1);
    assert(keys(typed) == 2);
    assert_key(&typed[0], 'A', 0);
    assert_key(&typed[1], 'B', 1);
    assert(keys(typed) == 2);
    assert_key(&typed[0], 'B', 0);
    assert_key(&typed[1], 0x0d, 1);
    assert(keys(typed) == 1);
    assert_key(&typed[0], 0x0d, 0);
    assert(!keys(typed) && osk.state == PW_OSK_CLOSED);

    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "ll");
    assert(keys(typed) == 1 && typed[0].vk == 'L' && typed[0].down);
    assert(keys(typed) == 1 && typed[0].vk == 'L' && !typed[0].down);   /* a tick up */
    assert(keys(typed) == 1 && typed[0].vk == 'L' && typed[0].down);
    assert(keys(typed) == 2 && !typed[0].down && typed[1].vk == 0x0d);
    assert(keys(typed) == 1 && !keys(typed));

    /* "aA": the same key, unshifted then shifted, is let go in between too. */
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "aA");
    assert_held_a_tick();
    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "Hello, World!! ~~ a  b\tc");
    assert_held_a_tick();
    assert(osk.state == PW_OSK_CLOSED);

    pw_osk_open(&osk);
    pw_osk_opened(&osk, 1);
    pw_osk_finished(&osk, "A");
    assert(!pw_osk_keys(&osk, typed, PW_OSK_KEYS_PER_TICK - 1) && osk.state == PW_OSK_TYPING);
    assert(keys(typed) == 2 && typed[1].vk == 'A');
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
    count = type_all(typed, sizeof(typed) / sizeof(typed[0]), NULL);
    assert(count == 6);
    assert_key(&typed[0], 'A', 1);
    assert_key(&typed[1], 'A', 0);
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
    count = type_all(typed, sizeof(typed) / sizeof(typed[0]), NULL);
    assert(count == 2 * (PW_OSK_TEXT - 1) + 2);
    assert(osk.state == PW_OSK_CLOSED);
}

/* A dialog that does not come up gives the pad straight back. */
static void test_refused(void)
{
    PwOskKey typed[4];

    memset(&osk, 0, sizeof(osk));
    held = 0;
    assert(tick2(TOUCHPAD, 1) && pw_osk_request(&osk));
    pw_osk_opened(&osk, 0);
    assert(osk.state == PW_OSK_CLOSED && !osk.opened);
    assert(!tick(TOUCHPAD));
    assert_view(0, 0, 0, 0);            /* the click stays hidden from the game */
    pw_osk_finished(&osk, "ignored");   /* no dialog was shown */
    assert(osk.state == PW_OSK_CLOSED && !keys(typed));
}

static void test_bad_arguments(void)
{
    PwOskKey typed[4];
    PwOskView ignored;

    memset(&osk, 0, sizeof(osk));
    assert(!pw_osk_tick(NULL, 0, 0, 0, 0, &ignored));
    assert(!pw_osk_tick(&osk, 0, 0, 0, 0, NULL));
    /* A gesture with no touchpad press this tick opens nothing. */
    assert(!pw_osk_tick(&osk, 0, 0, 0, 1, &ignored) && osk.state == PW_OSK_CLOSED);
    assert(!pw_osk_request(NULL));
    pw_osk_open(NULL);
    pw_osk_opened(NULL, 1);
    pw_osk_finished(NULL, "x");
    assert(!pw_osk_keys(NULL, typed, sizeof(typed) / sizeof(typed[0])));
    assert(!pw_osk_keys(&osk, NULL, PW_OSK_KEYS_PER_TICK));
}

int main(void)
{
    test_characters();
    test_gesture_and_typing();
    test_paced();
    test_unmappable();
    test_long_line();
    test_refused();
    test_bad_arguments();
    return 0;
}
