/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_game_menu.h"
#include <string.h>

/* Lines in order; only those with an action can take the cursor. */
enum { ACTION_NONE, ACTION_CLOSE };
static const struct { const char *text; int action; } lines[] = {
    { "No cheats for this game yet", ACTION_NONE },
    { "Close", ACTION_CLOSE },
};
enum { LINE_COUNT = sizeof(lines) / sizeof(lines[0]) };

static int selectable(int line) { return line >= 0 && line < LINE_COUNT && lines[line].action != ACTION_NONE; }

/* The next selectable line from the cursor in direction step, wrapping. */
static int move(int cursor, int step)
{
    for (int i = 1; i <= LINE_COUNT; i++) {
        int line = ((cursor + step * i) % LINE_COUNT + LINE_COUNT) % LINE_COUNT;
        if (selectable(line)) return line;
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
    for (int i = 0; i < LINE_COUNT; i++) strcpy(snapshot->line[i], lines[i].text);
}

int pw_game_menu_tick(PwGameMenu *menu, uint32_t held, uint32_t pressed, uint32_t released,
                      struct pw_title_menu *snapshot, PwGameMenuView *view)
{
    int combo = (held & PW_GAME_MENU_COMBO) == PW_GAME_MENU_COMBO && (pressed & PW_GAME_MENU_COMBO);
    uint32_t game;

    if (!menu || !snapshot || !view) return 0;
    if (!menu->open) {
        if (combo) {
            menu->open = 1;
            menu->cursor = move(-1, 1);
        }
    } else if (combo || (pressed & PW_GAME_MENU_CIRCLE)) {
        menu->open = 0;
    } else {
        if (pressed & PW_GAME_MENU_UP) menu->cursor = move(menu->cursor, -1);
        if (pressed & PW_GAME_MENU_DOWN) menu->cursor = move(menu->cursor, 1);
        if ((pressed & PW_GAME_MENU_CROSS) && selectable(menu->cursor) &&
            lines[menu->cursor].action == ACTION_CLOSE)
            menu->open = 0;
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
