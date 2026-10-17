/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_TITLE_MENU_H
#define PW_TITLE_MENU_H
#include <stdint.h>

/* The title's in-game menu as a game-side renderer draws it. The title owns
 * the menu: it reads the DualSense, keeps the cursor and the text, and
 * stores a snapshot in the sink (pw_wine_set_menu). Code inside the game
 * (DXVK's HUD) asks for the snapshot once a frame with
 * NtQueryInformationProcess(NtCurrentProcess(), PW_TITLE_MENU_PROCESS_INFO)
 * and draws it over the game's image. A Wine without the class fails the
 * query, and the renderer then has nothing to draw.
 *
 * Pointer-free wire layout, identical for PE32, PE64 and the Unix provider.
 * Text is ASCII (the HUD font has no other glyphs), NUL-terminated. */
enum { PW_TITLE_MENU_LINES=16,PW_TITLE_MENU_COLUMNS=48 };
struct pw_title_menu
{
    uint32_t version;       /* PW_TITLE_MENU_VERSION */
    uint32_t sequence;      /* the sink's: changes whenever the snapshot does */
    uint32_t open;          /* 0: draw nothing; the rest is then zero */
    uint32_t lines;         /* lines in use, at most PW_TITLE_MENU_LINES */
    int32_t selected;       /* the highlighted line, or -1 */
    char title[PW_TITLE_MENU_COLUMNS];
    char line[PW_TITLE_MENU_LINES][PW_TITLE_MENU_COLUMNS];
};
#define PW_TITLE_MENU_PROCESS_INFO 0x50574D01u
#define PW_TITLE_MENU_VERSION 1u

#endif
