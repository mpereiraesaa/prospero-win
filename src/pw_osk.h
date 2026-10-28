/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_OSK_H
#define PW_OSK_H
#include <stddef.h>
#include <stdint.h>

/*
 * The console's on-screen keyboard as a game's typing.
 *
 * A game that wants text (a chat line, a save name, a console command) has
 * no keyboard unless one is plugged in. A two-finger touchpad click opens
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
 *   pw_osk_tick()      the pad: opens on the gesture, and keeps the buttons
 *                      the keyboard took away from the game
 *   pw_osk_request()   1 when the dialog has to be opened now
 *   pw_osk_opened()    whether it came up
 *   pw_osk_finished()  the accepted line, or NULL when it was cancelled
 *   pw_osk_keys()      the next key events to post, a key change a tick
 *
 * The gesture is the touchpad click made with two fingers. The pad says
 * how many fingers began the click in the click's own sample, so it is
 * taken from the game in the tick it happens: no button is held back to
 * wait for a second one, and nothing of it reaches the game.
 *
 * The keys are those of a US keyboard, which is what Wine's PS5 driver
 * reports (native/pw_hid.c maps the USB layout the same way): a character
 * outside printable ASCII is dropped rather than typed wrongly.
 */

enum {
    /* The DualSense's touchpad click as scePadRead reports it. */
    PW_OSK_TOUCHPAD = 0x100000u,
    /* The accepted line, NUL-terminated; the dialog is asked for at most
     * PW_OSK_TEXT - 1 characters. */
    PW_OSK_TEXT = 256,
    /* Most key events one tick posts: the last character's key and Shift
     * released, this one's Shift and key pressed. */
    PW_OSK_KEYS_PER_TICK = 4,
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
    PwOskKey up[2];         /* keys pressed last tick, released this one */
    unsigned ups;
    unsigned long opened, cancelled, typed;   /* lines, for the log */
} PwOsk;

/* The pad as the game should see it this tick, as the in-game menu's view. */
typedef struct PwOskView {
    uint32_t held, pressed, released;
    int neutral;            /* 1: sticks centred and triggers released */
} PwOskView;

/* One tick of the title's loop with the pad's held buttons and the edges
 * since the last tick; gesture is 1 when a touchpad click made with two
 * fingers began this tick. Fills *view and returns 1 while the keyboard
 * has the pad (it is being opened or is up). While it does the game sees
 * nothing held; buttons still down when it goes away stay hidden from the
 * game until they are released. A two-finger click is never the game's,
 * even while the keyboard is typing. */
int pw_osk_tick(PwOsk *osk, uint32_t held, uint32_t pressed, uint32_t released, int gesture,
                PwOskView *view);

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

/* The key events to post this tick, at most max (PW_OSK_KEYS_PER_TICK
 * always fits): last tick's key released, and the next character's (or
 * the closing Enter's) pressed. Each key stays down for a whole tick, so a
 * game that reads its keyboard once a frame sees it, and a key typed twice
 * in a row is let go for a tick in between. Returns how many were
 * written, 0 when there is nothing left to type. */
size_t pw_osk_keys(PwOsk *osk, PwOskKey *out, size_t max);

/* The Windows virtual key of a printable ASCII character on a US
 * keyboard, with *shift set when it is typed with Shift held; 0 when the
 * character has no key. */
uint16_t pw_osk_character_vk(char character, int *shift);

#endif
