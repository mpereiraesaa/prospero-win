/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_hid.h"

/* Keyboard-page usages 0x04..0x65 (USB HID Usage Tables, section 10) to
 * Windows virtual keys; 0 has none. */
static const uint8_t usage_vk[0x66] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E', [0x09] = 'F',
    [0x0a] = 'G', [0x0b] = 'H', [0x0c] = 'I', [0x0d] = 'J', [0x0e] = 'K', [0x0f] = 'L',
    [0x10] = 'M', [0x11] = 'N', [0x12] = 'O', [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R',
    [0x16] = 'S', [0x17] = 'T', [0x18] = 'U', [0x19] = 'V', [0x1a] = 'W', [0x1b] = 'X',
    [0x1c] = 'Y', [0x1d] = 'Z',
    [0x1e] = '1', [0x1f] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6',
    [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x28] = 0x0d,  /* Enter */      [0x29] = 0x1b,  /* Escape */
    [0x2a] = 0x08,  /* Backspace */  [0x2b] = 0x09,  /* Tab */
    [0x2c] = 0x20,  /* Space */      [0x2d] = 0xbd,  /* - */
    [0x2e] = 0xbb,  /* = */          [0x2f] = 0xdb,  /* [ */
    [0x30] = 0xdd,  /* ] */          [0x31] = 0xdc,  /* \ */
    [0x32] = 0xdc,  /* non-US # */   [0x33] = 0xba,  /* ; */
    [0x34] = 0xde,  /* ' */          [0x35] = 0xc0,  /* ` */
    [0x36] = 0xbc,  /* , */          [0x37] = 0xbe,  /* . */
    [0x38] = 0xbf,  /* / */          [0x39] = 0x14,  /* Caps Lock */
    [0x3a] = 0x70, [0x3b] = 0x71, [0x3c] = 0x72, [0x3d] = 0x73, [0x3e] = 0x74, [0x3f] = 0x75,
    [0x40] = 0x76, [0x41] = 0x77, [0x42] = 0x78, [0x43] = 0x79, [0x44] = 0x7a, [0x45] = 0x7b,
    [0x46] = 0x2c,  /* Print Screen */ [0x47] = 0x91,  /* Scroll Lock */
    [0x48] = 0x13,  /* Pause */      [0x49] = 0x2d,  /* Insert */
    [0x4a] = 0x24,  /* Home */       [0x4b] = 0x21,  /* Page Up */
    [0x4c] = 0x2e,  /* Delete */     [0x4d] = 0x23,  /* End */
    [0x4e] = 0x22,  /* Page Down */  [0x4f] = 0x27,  /* Right */
    [0x50] = 0x25,  /* Left */       [0x51] = 0x28,  /* Down */
    [0x52] = 0x26,  /* Up */         [0x53] = 0x90,  /* Num Lock */
    [0x54] = 0x6f,  /* keypad / */   [0x55] = 0x6a,  /* keypad * */
    [0x56] = 0x6d,  /* keypad - */   [0x57] = 0x6b,  /* keypad + */
    [0x58] = 0x0d,  /* keypad Enter */
    [0x59] = 0x61, [0x5a] = 0x62, [0x5b] = 0x63, [0x5c] = 0x64, [0x5d] = 0x65,
    [0x5e] = 0x66, [0x5f] = 0x67, [0x60] = 0x68, [0x61] = 0x69, [0x62] = 0x60,
    [0x63] = 0x6e,  /* keypad . */   [0x64] = 0xe2,  /* non-US \ */
    [0x65] = 0x5d,  /* Application */
};

/* Modifier bits to virtual keys: Ctrl, Shift, Alt, Windows, left then
 * right. The generic keys (VK_CONTROL...) are what games test. */
static const uint8_t modifier_vk[8] = { 0x11, 0x10, 0x12, 0x5b, 0x11, 0x10, 0x12, 0x5c };

uint16_t pw_hid_usage_vk(uint8_t usage)
{
    return usage < sizeof(usage_vk) ? usage_vk[usage] : 0;
}

static int holds(const PwHidKeyboard *k, uint8_t usage)
{
    for (size_t i = 0; i < PW_HID_KEYS; i++)
        if (k->keys[i] == usage) return 1;
    return 0;
}

/* Ctrl, Shift and Alt share one virtual key for both sides, so theirs is
 * still held while the other side is; the Windows keys have one each. */
static int modifier_held(uint8_t modifiers, unsigned bit)
{
    if ((bit & 3u) == 3u) return modifiers >> bit & 1u;
    return (modifiers >> bit & 1u) || (modifiers >> ((bit + 4) & 7) & 1u);
}

static size_t put(PwHidEvent *out, size_t count, size_t capacity, uint8_t kind, uint8_t down,
                  uint16_t code)
{
    if (count < capacity) out[count] = (PwHidEvent){ kind, down, code };
    return count + 1;
}

size_t pw_hid_keyboard_events(const PwHidKeyboard *previous, const PwHidKeyboard *current,
                              PwHidEvent *out, size_t capacity)
{
    size_t count = 0;

    if (!previous || !current || !out) return 0;
    /* Releases first, so a key moved to another slot is not released after
     * its press. Usages 0..3 are "no key" and error states. */
    for (size_t i = 0; i < PW_HID_KEYS; i++) {
        uint8_t usage = previous->keys[i];
        if (usage > 3 && pw_hid_usage_vk(usage) && !holds(current, usage))
            count = put(out, count, capacity, PW_HID_EVENT_KEY, 0, pw_hid_usage_vk(usage));
    }
    for (unsigned bit = 0; bit < 8; bit++) {
        if ((previous->modifiers >> bit & 1u) && !(current->modifiers >> bit & 1u) &&
            !modifier_held(current->modifiers, bit))
            count = put(out, count, capacity, PW_HID_EVENT_KEY, 0, modifier_vk[bit]);
    }
    for (unsigned bit = 0; bit < 8; bit++) {
        if (!(previous->modifiers >> bit & 1u) && (current->modifiers >> bit & 1u) &&
            !modifier_held(previous->modifiers, bit))
            count = put(out, count, capacity, PW_HID_EVENT_KEY, 1, modifier_vk[bit]);
    }
    for (size_t i = 0; i < PW_HID_KEYS; i++) {
        uint8_t usage = current->keys[i];
        if (usage > 3 && pw_hid_usage_vk(usage) && !holds(previous, usage))
            count = put(out, count, capacity, PW_HID_EVENT_KEY, 1, pw_hid_usage_vk(usage));
    }
    return count < capacity ? count : capacity;
}

size_t pw_hid_mouse_button_events(uint32_t previous, uint32_t current, PwHidEvent *out,
                                  size_t capacity)
{
    static const uint32_t masks[3] = { PW_HID_MOUSE_LEFT, PW_HID_MOUSE_RIGHT, PW_HID_MOUSE_MIDDLE };
    size_t count = 0;

    if (!out) return 0;
    for (uint16_t i = 0; i < 3; i++)
        if ((previous ^ current) & masks[i])
            count = put(out, count, capacity, PW_HID_EVENT_BUTTON, (current & masks[i]) != 0, i);
    return count < capacity ? count : capacity;
}
