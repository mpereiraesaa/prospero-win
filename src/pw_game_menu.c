/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_game_menu.h"
#include <string.h>

/* Lines in order; only those with an action can take the cursor. Without
 * the console's keyboard its line says there is nothing else yet. */
enum { ACTION_NONE, ACTION_CLOSE, ACTION_KEYBOARD };
typedef struct MenuLine { const char *text; int action; } MenuLine;
static const MenuLine with_keyboard[] = {
    { "Keyboard input", ACTION_KEYBOARD },
    { "Close", ACTION_CLOSE },
};
static const MenuLine without_keyboard[] = {
    { "No cheats for this game yet", ACTION_NONE },
    { "Close", ACTION_CLOSE },
};
enum { LINE_COUNT = sizeof(with_keyboard) / sizeof(with_keyboard[0]) };
_Static_assert(sizeof(without_keyboard) == sizeof(with_keyboard), "both menus have the same lines");

static const MenuLine *lines_of(const PwGameMenu *menu)
{
    return menu->keyboard ? with_keyboard : without_keyboard;
}

static int selectable(const PwGameMenu *menu, int line)
{
    return line >= 0 && line < LINE_COUNT && lines_of(menu)[line].action != ACTION_NONE;
}

/* The next selectable line from the cursor in direction step, wrapping. */
static int move(const PwGameMenu *menu, int cursor, int step)
{
    for (int i = 1; i <= LINE_COUNT; i++) {
        int line = ((cursor + step * i) % LINE_COUNT + LINE_COUNT) % LINE_COUNT;
        if (selectable(menu, line)) return line;
    }
    return cursor;
}

static void snapshot_of(const PwGameMenu *menu, struct pw_title_menu *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = PW_TITLE_MENU_VERSION;
    if (!menu->open) return;
    snapshot->open = 1;
    snapshot->lines = LINE_COUNT;
    snapshot->selected = menu->cursor;
    strcpy(snapshot->title, "prospero-win");
    for (int i = 0; i < LINE_COUNT; i++) strcpy(snapshot->line[i], lines_of(menu)[i].text);
}

int pw_game_menu_tick(PwGameMenu *menu, uint32_t held, uint32_t pressed, uint32_t released,
                      struct pw_title_menu *snapshot, PwGameMenuView *view)
{
    int combo = (held & PW_GAME_MENU_COMBO) == PW_GAME_MENU_COMBO && (pressed & PW_GAME_MENU_COMBO);
    uint32_t game;

    if (!menu || !snapshot || !view) return 0;
    view->action = PW_GAME_MENU_ACTION_NONE;
    if (!menu->open) {
        if (combo) {
            menu->open = 1;
            menu->cursor = move(menu, -1, 1);
        }
    } else if (combo || (pressed & PW_GAME_MENU_CIRCLE)) {
        menu->open = 0;
    } else {
        if (pressed & PW_GAME_MENU_UP) menu->cursor = move(menu, menu->cursor, -1);
        if (pressed & PW_GAME_MENU_DOWN) menu->cursor = move(menu, menu->cursor, 1);
        if ((pressed & PW_GAME_MENU_CROSS) && selectable(menu, menu->cursor)) {
            if (lines_of(menu)[menu->cursor].action == ACTION_KEYBOARD)
                view->action = PW_GAME_MENU_ACTION_KEYBOARD;
            menu->open = 0;             /* every entry closes the menu */
        }
    }
    snapshot_of(menu, snapshot);

    menu->withheld = menu->open ? ~0u : menu->withheld & held;
    game = held & ~menu->withheld;
    view->held = game;
    view->pressed = (pressed & ~menu->withheld) | (game & ~menu->game_held);
    /* A release is the game's only for a button it saw down, or one pressed
     * and let go within this tick that it is shown pressed. */
    view->released = (menu->game_held & ~game) | (released & pressed & ~menu->withheld & ~game);
    view->neutral = menu->open;
    menu->game_held = game;
    return menu->open;
}
