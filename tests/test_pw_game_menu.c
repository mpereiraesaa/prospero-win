/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_game_menu.h"
#include <assert.h>
#include <string.h>

enum { L3 = PW_GAME_MENU_L3, R3 = PW_GAME_MENU_R3, UP = PW_GAME_MENU_UP, DOWN = PW_GAME_MENU_DOWN,
       CROSS = PW_GAME_MENU_CROSS, CIRCLE = PW_GAME_MENU_CIRCLE, SQUARE = 0x8000u, R2 = 0x200u };

static PwGameMenu menu;
static struct pw_title_menu shown;
static PwGameMenuView view;
static uint32_t held;

/* One tick from the previous held set to `now`, edges derived as the pad
 * core does. */
static int tick(uint32_t now)
{
    uint32_t pressed = now & ~held, released = held & ~now;
    held = now;
    return pw_game_menu_tick(&menu, now, pressed, released, &shown, &view);
}

static void assert_view(uint32_t h, uint32_t p, uint32_t r, int neutral)
{
    assert(view.held == h && view.pressed == p && view.released == r && view.neutral == neutral);
}

int main(void)
{
    struct pw_title_menu snapshot;
    menu.keyboard = 1;                      /* the title has the console's keyboard */
    /* Closed: the game gets the pad as it is, edges included. */
    assert(!tick(SQUARE));
    assert_view(SQUARE, SQUARE, 0, 0);
    assert(!shown.open && shown.version == PW_TITLE_MENU_VERSION && !shown.lines);
    assert(!tick(SQUARE | L3));
    assert_view(SQUARE | L3, L3, 0, 0);     /* L3 alone is the game's */

    /* L3+R3 opens it: the game sees everything it held released. */
    assert(tick(SQUARE | L3 | R3));
    assert_view(0, 0, SQUARE | L3, 1);
    assert(shown.open && shown.lines == 2 && !strcmp(shown.title, "prospero-win"));
    assert(!strcmp(shown.line[0], "Keyboard input") && !strcmp(shown.line[1], "Close"));
    assert(shown.selected == 0);            /* the first line with an action */

    /* While open nothing reaches the game; the d-pad wraps around. */
    assert(tick(0));
    assert_view(0, 0, 0, 1);
    assert(tick(UP) && shown.selected == 1);
    assert(tick(0) && tick(DOWN) && shown.selected == 0);
    assert(tick(0) && tick(DOWN) && shown.selected == 1);
    assert(!tick(CROSS | R2));
    assert(!menu.open && !shown.open);      /* Close picked */
    assert(view.action == PW_GAME_MENU_ACTION_NONE);
    assert_view(0, 0, 0, 0);                /* Cross, R2 down and Down let go: hidden */
    assert(!tick(CROSS | R2 | SQUARE));
    assert_view(SQUARE, SQUARE, 0, 0);      /* a new press is the game's */
    assert(!tick(SQUARE | R2));
    assert_view(SQUARE, 0, 0, 0);           /* Cross let go: never the game's */
    assert(!tick(SQUARE));
    assert(!tick(SQUARE | R2));
    assert_view(SQUARE | R2, R2, 0, 0);     /* released once, R2 is the game's again */

    /* Circle closes, and so does the combo again; holding it does not reopen. */
    assert(!tick(0));
    assert(tick(L3 | R3) && tick(0) && !tick(CIRCLE));
    assert(!menu.open && !tick(0));
    assert(tick(L3 | R3) && tick(L3 | R3));  /* held, not pressed again: stays open */
    assert(tick(L3) && !tick(L3 | R3));      /* R3 pressed again closes it */
    assert(!tick(L3 | R3 | CROSS));
    assert_view(CROSS, CROSS, 0, 0);         /* the combo stays hidden */
    assert(!tick(0) && tick(R3 | L3) && menu.open);

    /* A batch that pressed and released a button within the tick still
     * reaches the game once the menu is closed. */
    assert(!tick(CIRCLE) && !menu.open && !tick(0));
    assert(!pw_game_menu_tick(&menu, 0, SQUARE, SQUARE, &shown, &view));
    assert_view(0, SQUARE, SQUARE, 0);

    /* Keyboard input closes the menu and asks the title for the console's
     * on-screen keyboard, for that tick only. */
    held = 0;
    assert(tick(L3 | R3) && shown.selected == 0);
    assert(tick(0) && view.action == PW_GAME_MENU_ACTION_NONE);
    assert(!tick(CROSS) && view.action == PW_GAME_MENU_ACTION_KEYBOARD);
    assert(!menu.open && !shown.open);
    assert(!tick(0) && view.action == PW_GAME_MENU_ACTION_NONE);

    /* Without the console's keyboard its line is the placeholder, which the
     * cursor skips: only Close is offered, and nothing asks for a keyboard. */
    memset(&menu, 0, sizeof(menu));
    held = 0;
    assert(tick(L3 | R3));
    assert(!strcmp(shown.line[0], "No cheats for this game yet") && !strcmp(shown.line[1], "Close"));
    assert(shown.selected == 1);
    assert(tick(0) && tick(UP) && shown.selected == 1);
    assert(tick(0) && !tick(CROSS) && view.action == PW_GAME_MENU_ACTION_NONE && !menu.open);

    /* Bad arguments do nothing. */
    assert(!pw_game_menu_tick(NULL, 0, 0, 0, &snapshot, &view));
    assert(!pw_game_menu_tick(&menu, 0, 0, 0, NULL, &view));
    assert(!pw_game_menu_tick(&menu, 0, 0, 0, &snapshot, NULL));
    return 0;
}
