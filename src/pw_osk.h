/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_OSK_H
#define PW_OSK_H
#include <stddef.h>
#include <stdint.h>

/*
 * The console's on-screen keyboard as a game's typing.
 *
 * A game that wants text (a chat line, a save name, a console command) has
 * no keyboard unless one is plugged in. Options + the touchpad click opens
 * the PS5's own keyboard over the game instead; what is written there
 * reaches the game as key presses once it is accepted, with Enter at the
 * end, so a chat line is sent as it would be from a PC keyboard. The
 * in-game menu's "Keyboard input" opens the same keyboard.
 *
 * This is the portable half: the combo, the state the title drives, and
 * the keys a line of text becomes. The console half, which shows the
 * system dialog, is native/pw_osk_ps5.c.
 *
 * The title drives it in this order, once a tick:
 *
 *   pw_osk_tick()      the pad: opens on the combo, and keeps the buttons
 *                      the keyboard took away from the game
 *   pw_osk_request()   1 when the dialog has to be opened now
 *   pw_osk_opened()    whether it came up
 *   pw_osk_finished()  the accepted line, or NULL when it was cancelled
 *   pw_osk_keys()      the next key events to post, a character a tick
 *
 * The keys are those of a US keyboard, which is what Wine's PS5 driver
 * reports (native/pw_hid.c maps the USB layout the same way): a character
 * outside printable ASCII is dropped rather than typed wrongly.
 */

enum {
    /* DualSense buttons as scePadRead reports them. */
    PW_OSK_OPTIONS = 0x8u, PW_OSK_TOUCHPAD = 0x100000u,
    PW_OSK_COMBO = PW_OSK_OPTIONS | PW_OSK_TOUCHPAD,
    /* The accepted line, NUL-terminated; the dialog is asked for at most
     * PW_OSK_TEXT - 1 characters. */
    PW_OSK_TEXT = 256,
    /* Shift and the key, pressed and released. */
    PW_OSK_KEYS_PER_CHARACTER = 4,
};

/* What the title does with the keyboard this tick. */
enum { PW_OSK_CLOSED = 0, PW_OSK_OPENING, PW_OSK_SHOWN, PW_OSK_TYPING };

/* One key change for the sink (PW_WINE_INPUT_KEY). */
typedef struct PwOskKey {
    uint16_t vk;
    uint8_t down;
} PwOskKey;

typedef struct PwOsk {
    int state;
    uint32_t withheld;      /* buttons the game must not see yet */
    uint32_t game_held;     /* what the game saw held last tick */
    char text[PW_OSK_TEXT]; /* the accepted line, while it is being typed */
    size_t at;              /* the next character of it */
    unsigned enter;         /* the closing Enter is still to be sent */
    unsigned long opened, cancelled, typed;   /* lines, for the log */
} PwOsk;

/* The pad as the game should see it this tick, as the in-game menu's view. */
typedef struct PwOskView {
    uint32_t held, pressed, released;
    int neutral;            /* 1: sticks centred and triggers released */
} PwOskView;

/* One tick of the title's loop with the pad's held buttons and the edges
 * since the last tick. Fills *view and returns 1 while the keyboard has
 * the pad (it is being opened or is up). While it does the game sees
 * nothing held; buttons still down when it goes away stay hidden from the
 * game until they are released, so the combo is never the game's. */
int pw_osk_tick(PwOsk *osk, uint32_t held, uint32_t pressed, uint32_t released, PwOskView *view);

/* Asks for the keyboard without the combo (the in-game menu's entry).
 * Ignored while it is already open. */
void pw_osk_open(PwOsk *osk);

/* 1 when the title must open the system dialog now. */
int pw_osk_request(const PwOsk *osk);

/* The dialog came up (opened), or could not be opened (0): then the pad is
 * the game's again. */
void pw_osk_opened(PwOsk *osk, int opened);

/* The dialog closed: text is the accepted line, or NULL when it was
 * cancelled or empty. A line longer than the buffer is typed as far as it
 * fits. */
void pw_osk_finished(PwOsk *osk, const char *text);

/* The key events to post this tick, at most max: one character's, then
 * Enter after the last one. Returns how many were written, 0 when there is
 * nothing to type. */
size_t pw_osk_keys(PwOsk *osk, PwOskKey *out, size_t max);

/* The Windows virtual key of a printable ASCII character on a US
 * keyboard, with *shift set when it is typed with Shift held; 0 when the
 * character has no key. */
uint16_t pw_osk_character_vk(char character, int *shift);

#endif
