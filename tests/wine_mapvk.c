/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* MapVirtualKeyExA with codes above 0xff, as a 32-bit game asks it: a game
 * that names a binding with MapVirtualKeyExA(code, MAPVK_VSC_TO_VK, layout)
 * for its own controller codes (0x100 and up) expects 0, "no translation";
 * Windows documents only a high byte of 0xe0 or 0xe1 (an extended scan code)
 * as valid, and Wine's user32 tests expect 0 for 0x011d and 0x021d under
 * MAPVK_VSC_TO_VK_EX. Every code 0x100-0x2ff must give 0 in both modes, while
 * plain and E0 scan codes still translate (Ctrl, Space, Shift, E0 1D). The key
 * names of the extended Right Arrow and Right Ctrl (GetKeyNameTextA, which
 * reaches the driver with 0x1xx codes) are printed so a caller can check
 * that they do not change. Only layout-independent results are asserted.
 * One line: MAPVK verdict=pass|fail ...; exit 0 on pass. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* A key name as one word: spaces become '_', nothing is "-". */
static void key_name(LONG lparam, char *out, int size)
{
    int n = GetKeyNameTextA(lparam, out, size);
    if (n <= 0) { strcpy(out, "-"); return; }
    for (int i = 0; i < n; i++) if (out[i] == ' ') out[i] = '_';
}

int main(void)
{
    HKL kl = GetKeyboardLayout(0);
    unsigned n1 = 0, bad1 = 0, n3 = 0, bad3 = 0, first1 = 0, first1_vk = 0, first3 = 0, first3_vk = 0;
    UINT ctrl1, ctrl3, space1, shift1, e0ctrl1, e0ctrl3;
    char right[64], rctrl[64], first1_text[32] = "-", first3_text[32] = "-";
    int pass;

    for (UINT code = 0x100; code <= 0x2ff; code++) {
        UINT r1 = MapVirtualKeyExA(code, MAPVK_VSC_TO_VK, kl);
        UINT r3 = MapVirtualKeyExA(code, MAPVK_VSC_TO_VK_EX, kl);
        n1++; n3++;
        if (r1) { if (!bad1++) { first1 = code; first1_vk = r1; } }
        if (r3) { if (!bad3++) { first3 = code; first3_vk = r3; } }
    }
    if (bad1) snprintf(first1_text, sizeof(first1_text), "0x%x:0x%x", first1, first1_vk);
    if (bad3) snprintf(first3_text, sizeof(first3_text), "0x%x:0x%x", first3, first3_vk);
    ctrl1 = MapVirtualKeyExA(0x1d, MAPVK_VSC_TO_VK, kl);
    ctrl3 = MapVirtualKeyExA(0x1d, MAPVK_VSC_TO_VK_EX, kl);
    space1 = MapVirtualKeyExA(0x39, MAPVK_VSC_TO_VK, kl);
    shift1 = MapVirtualKeyExA(0x2a, MAPVK_VSC_TO_VK, kl);
    e0ctrl1 = MapVirtualKeyExA(0xe01d, MAPVK_VSC_TO_VK, kl);
    e0ctrl3 = MapVirtualKeyExA(0xe01d, MAPVK_VSC_TO_VK_EX, kl);   /* recorded: Windows says VK_RCONTROL */
    key_name(0x014d0000, right, sizeof(right));    /* extended Right Arrow */
    key_name(0x011d0000, rctrl, sizeof(rctrl));    /* extended Right Ctrl */

    pass = !bad1 && !bad3 && ctrl1 == VK_CONTROL && ctrl3 == VK_LCONTROL && space1 == VK_SPACE &&
           shift1 == VK_SHIFT && e0ctrl1 == VK_CONTROL;
    printf("MAPVK verdict=%s high1=%u/%u high3=%u/%u first1=%s first3=%s ctrl1=0x%02x ctrl3=0x%02x space1=0x%02x "
           "shift1=0x%02x e0ctrl1=0x%02x e0ctrl3=0x%02x name_right=%s name_rctrl=%s\n",
           pass ? "pass" : "fail", bad1, n1, bad3, n3, first1_text, first3_text, ctrl1, ctrl3, space1, shift1,
           e0ctrl1, e0ctrl3, right, rctrl);
    fflush(stdout);
    return pass ? 0 : 1;
}
