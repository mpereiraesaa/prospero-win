/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_SPINNER_H
#define PW_SPINNER_H
/*
 * What the title shows while a game loads: a ring of dots, one bright and
 * the ones behind it fading, on black. A game shows nothing for seconds
 * while Wine starts and it loads; the title shows this instead until the
 * game's first frame with content (pw_spinner_frame_blank).
 */
#include <stddef.h>
#include <stdint.h>

enum { PW_SPINNER_WIDTH = 960, PW_SPINNER_HEIGHT = 540, PW_SPINNER_DOTS = 12 };

/* Draw step (any number; one step moves the bright dot one place) into a
 * B,G,R,X image of width x height, stride pixels a row. */
void pw_spinner_draw(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride, uint32_t step);

/* 1 when a B,G,R,X frame shows nothing: fewer than 8 of the points of a
 * 32 x 18 grid over it are more than 24 from black in any channel, so a
 * cursor or a stray pixel on black is still nothing. */
int pw_spinner_frame_blank(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride_bytes);
#endif
