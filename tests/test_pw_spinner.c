/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../src/pw_spinner.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t image[PW_SPINNER_WIDTH * PW_SPINNER_HEIGHT];

/* The brightest pixel's position, and how many pixels are lit. */
static void brightest(uint32_t *x, uint32_t *y, uint32_t *lit, uint32_t *peak)
{
    *lit = *peak = 0;
    for (uint32_t j = 0; j < PW_SPINNER_HEIGHT; j++)
        for (uint32_t i = 0; i < PW_SPINNER_WIDTH; i++) {
            uint32_t v = image[j * PW_SPINNER_WIDTH + i] & 0xff;
            if (v) (*lit)++;
            if (v > *peak) { *peak = v; *x = i; *y = j; }
        }
}

int main(void)
{
    uint32_t x, y, lit, peak, lit0;

    /* Step 0: the bright dot is the top one, the ring centred; grey, on
     * black, with the X byte clear. */
    memset(image, 0xab, sizeof(image));
    pw_spinner_draw(image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH, 0);
    brightest(&x, &y, &lit, &peak);
    assert(peak == 255 && x >= 470 && x <= 490 && y < 270 && y > 220);
    assert(image[0] == 0 && image[270 * PW_SPINNER_WIDTH + 480] == 0);
    assert((image[y * PW_SPINNER_WIDTH + x] & 0xffffff) == 0xffffff && !(image[y * PW_SPINNER_WIDTH + x] >> 24));
    lit0 = lit;
    /* Three steps on, the bright dot is a quarter turn round: to the right. */
    pw_spinner_draw(image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH, 3);
    brightest(&x, &y, &lit, &peak);
    assert(peak == 255 && x > 500 && y >= 260 && y <= 280 && lit == lit0);
    /* The step wraps. */
    pw_spinner_draw(image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH, 3 + 12 * 1000);
    brightest(&x, &y, &lit, &peak);
    assert(x > 500 && y >= 260 && y <= 280);

    /* Blank: black and near-black frames; not: the spinner, a white
     * frame, one lit pixel on the grid. */
    memset(image, 0, sizeof(image));
    assert(pw_spinner_frame_blank((const uint8_t *)image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH * 4));
    for (size_t i = 0; i < sizeof(image) / 4; i++) image[i] = 0x00101810u;
    assert(pw_spinner_frame_blank((const uint8_t *)image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH * 4));
    memset(image, 0xff, sizeof(image));
    assert(!pw_spinner_frame_blank((const uint8_t *)image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH * 4));
    /* A cursor's worth (one grid point lit) is still blank; eight are not. */
    memset(image, 0, sizeof(image));
    image[(PW_SPINNER_HEIGHT / 36) * PW_SPINNER_WIDTH + PW_SPINNER_WIDTH / 64] = 0x00400000u;
    assert(pw_spinner_frame_blank((const uint8_t *)image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH * 4));
    for (uint32_t c = 0; c < 8; c++)
        image[(PW_SPINNER_HEIGHT / 36) * PW_SPINNER_WIDTH + (2 * c + 1) * PW_SPINNER_WIDTH / 64] = 0x00400000u;
    assert(!pw_spinner_frame_blank((const uint8_t *)image, PW_SPINNER_WIDTH, PW_SPINNER_HEIGHT, PW_SPINNER_WIDTH * 4));
    assert(pw_spinner_frame_blank(NULL, 1, 1, 4));
    puts("loading spinner passed: ring of dots, the bright one moves and wraps, blank frames told from content");
    return 0;
}
