/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VIDEOOUT_TILE_H
#define PW_VIDEOOUT_TILE_H
#include <stddef.h>
#include <stdint.h>

/* The scanout's tiled layout: 512x128-pixel tiles, and inside a tile a
 * swizzle of the column and row bits. The swizzle of (x, y) is the XOR of
 * the swizzle of (x, 0) and of (0, y), so a pixel's index splits into a
 * part from its column and a part from its row: two tables replace the
 * per-pixel bit shuffling. */
enum { PW_VIDEOOUT_TILE_WIDTH = 1920, PW_VIDEOOUT_TILE_HEIGHT = 1080 };

static inline uint32_t pw_videoout_tile_offset(uint32_t x, uint32_t y)
{
    return ((x&1u)<<0)|(((x>>1)&1u)<<1)|(((y>>0)&1u)<<2)|(((y>>1)&1u)<<3)|
        (((y>>2)&1u)<<4)|(((x>>2)&1u)<<5)|((((x>>3)^(y>>3))&1u)<<6)|
        ((((x>>4)^(y>>4))&1u)<<7)|((((x>>6)^(y>>5))&1u)<<8)|
        ((((x>>5)^(y>>6))&1u)<<9)|(((y>>3)&1u)<<10)|(((x>>4)&1u)<<11)|
        (((y>>6)&1u)<<12)|(((x>>6)&1u)<<13)|(((x>>7)&1u)<<14)|(((x>>8)&1u)<<15);
}

/* The index of pixel (x, y) in the tiled scanout, computed directly. */
static inline size_t pw_videoout_tile_pixel(uint32_t x, uint32_t y)
{
    return (size_t)(x/512u)*(512u*128u)+(size_t)(y/128u)*(128u*PW_VIDEOOUT_TILE_WIDTH)+
        (pw_videoout_tile_offset(x%512u,0)^pw_videoout_tile_offset(0,y%128u));
}

/* Per-column and per-row parts: index = column_base[x] + row_base[y] +
 * (column_swizzle[x] ^ row_swizzle[y]). */
typedef struct PwVideoOutTiles {
    uint32_t column_base[PW_VIDEOOUT_TILE_WIDTH], column_swizzle[PW_VIDEOOUT_TILE_WIDTH];
    uint32_t row_base[PW_VIDEOOUT_TILE_HEIGHT], row_swizzle[PW_VIDEOOUT_TILE_HEIGHT];
} PwVideoOutTiles;

static inline void pw_videoout_tiles_init(PwVideoOutTiles *tiles)
{
    for (uint32_t x = 0; x < PW_VIDEOOUT_TILE_WIDTH; x++) {
        tiles->column_base[x] = x / 512u * (512u * 128u);
        tiles->column_swizzle[x] = pw_videoout_tile_offset(x % 512u, 0);
    }
    for (uint32_t y = 0; y < PW_VIDEOOUT_TILE_HEIGHT; y++) {
        tiles->row_base[y] = y / 128u * (128u * PW_VIDEOOUT_TILE_WIDTH);
        tiles->row_swizzle[y] = pw_videoout_tile_offset(0, y % 128u);
    }
}

static inline size_t pw_videoout_tiles_index(const PwVideoOutTiles *tiles, uint32_t x, uint32_t y)
{
    return (size_t)tiles->column_base[x] + tiles->row_base[y] +
           (tiles->column_swizzle[x] ^ tiles->row_swizzle[y]);
}
#endif
