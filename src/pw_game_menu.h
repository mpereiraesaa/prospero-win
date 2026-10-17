/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_GAME_MENU_H
#define PW_GAME_MENU_H
#include <stdint.h>
#include "../wine/ps5/menu/pw_title_menu.h"

/* The in-game menu a profile turns on with [display] menu = true. L3+R3
 * pressed together opens it over the game and closes it again; while it is
 * open the title keeps the DualSense from the game, the d-pad moves the
 * cursor, Cross picks the line and Circle closes. The game's renderer draws
 * the snapshot (DXVK's HUD, through ntdll's ProcessWineTitleMenu).
 *
 * The menu has no entries of its own yet: it says so and offers Close. */
typedef struct PwGameMenu {
    int open;
    int cursor;
    uint32_t withheld;      /* buttons the game must not see yet */
    uint32_t game_held;     /* what the game saw held last tick */
} PwGameMenu;

/* The pad as the game should see it this tick. */
typedef struct PwGameMenuView {
    uint32_t held, pressed, released;
    int neutral;            /* 1: sticks centred and triggers released */
} PwGameMenuView;

/* DualSense buttons the menu reads, as scePadRead reports them. */
enum { PW_GAME_MENU_L3 = 0x2u, PW_GAME_MENU_R3 = 0x4u, PW_GAME_MENU_UP = 0x10u,
       PW_GAME_MENU_DOWN = 0x40u, PW_GAME_MENU_CIRCLE = 0x2000u, PW_GAME_MENU_CROSS = 0x4000u,
       PW_GAME_MENU_COMBO = PW_GAME_MENU_L3 | PW_GAME_MENU_R3 };

/* One tick of the title's loop with the pad's held buttons and the edges
 * since the last tick. Fills *snapshot (closed when the menu is) and *view,
 * and returns 1 while the menu is open. While open the game sees nothing
 * held; buttons still down when the menu closes stay hidden from the game
 * until they are released, so the press that closed it is not the game's.
 * With nothing hidden the view is the pad's own. */
int pw_game_menu_tick(PwGameMenu *menu, uint32_t held, uint32_t pressed, uint32_t released,
                      struct pw_title_menu *snapshot, PwGameMenuView *view);

#endif
