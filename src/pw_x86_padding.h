/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_X86_PADDING_H
#define PW_X86_PADDING_H
#include <stddef.h>
#include <stdint.h>

/* Positive: complete NOP length; zero: another instruction; -1: truncated;
 * -2: invalid NOP encoding. No operand is evaluated or accessed. */
static inline int pw_x86_padding_length(const uint8_t *s, size_t bytes)
{
    size_t i = 0, displacement = 0;
    unsigned address16 = 0, mod, rm;
    uint8_t operand;
    while (i < bytes) {
        uint8_t p = s[i];
        if (p == 0x67) address16 = 1;
        else if (p != 0x66 && p != 0x26 && p != 0x2e && p != 0x36 &&
                 p != 0x3e && p != 0x64 && p != 0x65) break;
        if (++i >= 15) return -2;
    }
    if (i == bytes) return i ? -1 : 0;
    if (s[i] == 0x90) return (int)(i + 1);
    if (s[i] != 0x0f) return 0;
    if (i + 1 >= bytes) return -1;
    if (s[i + 1] != 0x1f) return 0;
    i += 2;
    if (i >= 15) return -2;
    if (i >= bytes) return -1;
    operand = s[i++];
    if (operand & 0x38) return -2; /* 0F 1F /0 only */
    mod = operand >> 6; rm = operand & 7;
    if (mod != 3) {
        if (address16) {
            displacement = mod == 1 ? 1 : mod == 2 || rm == 6 ? 2 : 0;
        } else {
            if (rm == 4) {
                if (i >= 15) return -2;
                if (i >= bytes) return -1;
                rm = s[i++] & 7;
            }
            displacement = mod == 1 ? 1 : mod == 2 || rm == 5 ? 4 : 0;
        }
    }
    if (i + displacement > 15) return -2;
    if (i + displacement > bytes) return -1;
    return (int)(i + displacement);
}
#endif
