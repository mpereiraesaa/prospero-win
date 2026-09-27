/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../native/pw_videoout_tile.h"
#include <assert.h>
#include <stdio.h>

static PwVideoOutTiles tiles;

int main(void)
{
    pw_videoout_tiles_init(&tiles);
    /* The tables give every pixel the index of the direct formula. */
    for (uint32_t y = 0; y < PW_VIDEOOUT_TILE_HEIGHT; y++)
        for (uint32_t x = 0; x < PW_VIDEOOUT_TILE_WIDTH; x++)
            assert(pw_videoout_tiles_index(&tiles, x, y) == pw_videoout_tile_pixel(x, y));
    assert(pw_videoout_tile_pixel(0, 0) == 0 && pw_videoout_tile_pixel(1, 0) == 1);
    assert(pw_videoout_tile_pixel(0, 1) == 4 && pw_videoout_tile_pixel(512, 0) == 512u * 128u);
    printf("videoout tiles passed: table indices equal the tiled formula for all %u pixels\n",
           PW_VIDEOOUT_TILE_WIDTH * PW_VIDEOOUT_TILE_HEIGHT);
    return 0;
}
