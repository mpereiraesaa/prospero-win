/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_spinner.h"
#include <string.h>

/* Where each dot sits on the ring, clockwise from the top: cos and sin of
 * -90, -60, ... 240 degrees. */
static const double ring_x[PW_SPINNER_DOTS] = {
    0.0, 0.5, 0.8660254037844386, 1.0, 0.8660254037844386, 0.5,
    0.0, -0.5, -0.8660254037844386, -1.0, -0.8660254037844386, -0.5,
};
static const double ring_y[PW_SPINNER_DOTS] = {
    -1.0, -0.8660254037844386, -0.5, 0.0, 0.5, 0.8660254037844386,
    1.0, 0.8660254037844386, 0.5, 0.0, -0.5, -0.8660254037844386,
};

void pw_spinner_draw(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride, uint32_t step)
{
    const double ring = height / 14.0, dot = height / 90.0;
    const double cx = width / 2.0, cy = height / 2.0;

    for (uint32_t y = 0; y < height; y++) memset(pixels + (size_t)y * stride, 0, (size_t)width * 4);
    for (uint32_t i = 0; i < PW_SPINNER_DOTS; i++) {
        /* The bright dot is step's; the ones it left fade behind it. */
        const uint32_t age = (step % PW_SPINNER_DOTS + PW_SPINNER_DOTS - i) % PW_SPINNER_DOTS;
        const uint32_t level = age * 20u >= 200u ? 55u : 255u - age * 20u;
        const double x0 = cx + ring * ring_x[i], y0 = cy + ring * ring_y[i];
        const int32_t top = (int32_t)(y0 - dot - 1), bottom = (int32_t)(y0 + dot + 1);
        const int32_t left = (int32_t)(x0 - dot - 1), right = (int32_t)(x0 + dot + 1);

        for (int32_t y = top < 0 ? 0 : top; y <= bottom && y < (int32_t)height; y++)
            for (int32_t x = left < 0 ? 0 : left; x <= right && x < (int32_t)width; x++) {
                const double dx = x + 0.5 - x0, dy = y + 0.5 - y0;
                if (dx * dx + dy * dy <= dot * dot)
                    pixels[(size_t)y * stride + (uint32_t)x] = level * 0x010101u;
            }
    }
}

int pw_spinner_frame_blank(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride_bytes)
{
    enum { COLUMNS = 32, ROWS = 18, LIMIT = 24, LIT = 8 };
    uint32_t lit = 0;

    if (!pixels || !width || !height) return 1;
    for (uint32_t r = 0; r < ROWS; r++) {
        const uint8_t *row = pixels + (size_t)((r * 2u + 1u) * height / (ROWS * 2u)) * stride_bytes;
        for (uint32_t c = 0; c < COLUMNS; c++) {
            const uint8_t *p = row + (size_t)((c * 2u + 1u) * width / (COLUMNS * 2u)) * 4u;
            if ((p[0] > LIMIT || p[1] > LIMIT || p[2] > LIMIT) && ++lit >= LIT) return 0;
        }
    }
    return 1;
}
