/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_osk.h"
#include <string.h>

enum { VK_TAB = 0x09, VK_RETURN = 0x0d, VK_SHIFT = 0x10, VK_SPACE = 0x20 };

uint16_t pw_osk_character_vk(char character, int *shift)
{
    /* What a US keyboard types with Shift held, and the key that types it. */
    static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    static const char plain[] = "`1234567890-=[]\\;',./";
    const char *found;
    char key = character;
    int held = 0;

    if (shift) *shift = 0;
    if (key >= 'a' && key <= 'z') return (uint16_t)(key - 'a' + 'A');
    if (key >= 'A' && key <= 'Z') { if (shift) *shift = 1; return (uint16_t)key; }
    if (key == ' ') return VK_SPACE;
    if (key == '\t') return VK_TAB;
    if (key == '\n' || key == '\r') return VK_RETURN;
    if (key && (found = strchr(shifted, key)) != NULL) {
        held = 1;
        key = plain[found - shifted];
    }
    if (shift) *shift = held;
    if (key >= '0' && key <= '9') return (uint16_t)key;
    switch (key) {                      /* the OEM keys, US layout */
    case ';': return 0xba;
    case '=': return 0xbb;
    case ',': return 0xbc;
    case '-': return 0xbd;
    case '.': return 0xbe;
    case '/': return 0xbf;
    case '`': return 0xc0;
    case '[': return 0xdb;
    case '\\': return 0xdc;
    case ']': return 0xdd;
    case '\'': return 0xde;
    }
    if (shift) *shift = 0;
    return 0;
}

void pw_osk_open(PwOsk *osk)
{
    if (osk && osk->state == PW_OSK_CLOSED) osk->state = PW_OSK_OPENING;
}

int pw_osk_request(const PwOsk *osk)
{
    return osk && osk->state == PW_OSK_OPENING;
}

void pw_osk_opened(PwOsk *osk, int opened)
{
    if (!osk || osk->state != PW_OSK_OPENING) return;
    osk->state = opened ? PW_OSK_SHOWN : PW_OSK_CLOSED;
    if (opened) osk->opened++;
}

void pw_osk_finished(PwOsk *osk, const char *text)
{
    size_t length = 0;

    if (!osk || osk->state != PW_OSK_SHOWN) return;
    if (text) {
        while (length + 1 < PW_OSK_TEXT && text[length]) length++;
        memcpy(osk->text, text, length);
    }
    osk->text[length] = '\0';
    osk->at = 0;
    osk->enter = length != 0;
    osk->state = length ? PW_OSK_TYPING : PW_OSK_CLOSED;
    if (length) osk->typed++;
    else osk->cancelled++;
}

int pw_osk_tick(PwOsk *osk, uint32_t held, uint32_t pressed, uint32_t released, PwOskView *view)
{
    int combo = (held & PW_OSK_COMBO) == PW_OSK_COMBO && (pressed & PW_OSK_COMBO);
    uint32_t game;
    int taken;

    if (!osk || !view) return 0;
    if (combo) pw_osk_open(osk);
    taken = osk->state == PW_OSK_OPENING || osk->state == PW_OSK_SHOWN;

    osk->withheld = taken ? ~0u : osk->withheld & held;
    game = held & ~osk->withheld;
    view->held = game;
    view->pressed = (pressed & ~osk->withheld) | (game & ~osk->game_held);
    /* A release is the game's only for a button it saw down, or one pressed
     * and let go within this tick that it is shown pressed. */
    view->released = (osk->game_held & ~game) | (released & pressed & ~osk->withheld & ~game);
    view->neutral = taken;
    osk->game_held = game;
    return taken;
}

size_t pw_osk_keys(PwOsk *osk, PwOskKey *out, size_t max)
{
    size_t count = 0;

    if (!osk || !out || osk->state != PW_OSK_TYPING) return 0;
    /* One character a tick: a game reads its keyboard once a frame, and a
     * whole line posted at once would reach it as a single press. */
    while (!count && osk->at + 1 < PW_OSK_TEXT && osk->text[osk->at]) {
        int shift = 0;
        uint16_t vk = pw_osk_character_vk(osk->text[osk->at], &shift);
        size_t needed = shift ? 4u : 2u;

        osk->at++;
        if (!vk) continue;              /* no key types it; the rest still does */
        if (needed > max) { osk->at--; return 0; }
        if (shift) out[count++] = (PwOskKey){ VK_SHIFT, 1 };
        out[count++] = (PwOskKey){ vk, 1 };
        out[count++] = (PwOskKey){ vk, 0 };
        if (shift) out[count++] = (PwOskKey){ VK_SHIFT, 0 };
    }
    if (!count && osk->enter) {
        if (max < 2) return 0;
        out[count++] = (PwOskKey){ VK_RETURN, 1 };
        out[count++] = (PwOskKey){ VK_RETURN, 0 };
        osk->enter = 0;
    }
    if (!count) {
        osk->state = PW_OSK_CLOSED;
        osk->at = 0;
        osk->text[0] = '\0';
    }
    return count;
}
