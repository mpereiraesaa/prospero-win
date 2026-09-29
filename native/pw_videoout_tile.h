/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VIDEOOUT_TILE_H
#define PW_VIDEOOUT_TILE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../src/pw_present.h"

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

/* The scanout is registered A8B8G8R8 (red in the low byte) while GDI, Wine
 * and the launcher hand over B,G,R,X pixels: swap red and blue. */
static inline uint32_t pw_videoout_rgbx(uint32_t bgrx)
{
    return 0xff000000u | (bgrx & 0x0000ff00u) | ((bgrx >> 16) & 0xffu) | ((bgrx & 0xffu) << 16);
}

/* The pixel for a scanout: A8B8G8R8 (the title's own), or B8G8R8A8 (bgra,
 * the Vulkan driver's, whose smaller sets VideoOut scales to the mode). */
static inline uint32_t pw_videoout_scanout_pixel(uint32_t bgrx, int bgra)
{
    return bgra ? 0xff000000u | bgrx : pw_videoout_rgbx(bgrx);
}

/* Scratch for pw_videoout_tiles_scale, kept off the stack: the source
 * column of each shown column, and scanout rows converted ahead of tiling
 * (four, and one of background). */
typedef struct PwVideoOutScaleRows {
    uint32_t source_column[PW_VIDEOOUT_TILE_WIDTH];
    uint32_t row[5][PW_VIDEOOUT_TILE_WIDTH];
} PwVideoOutScaleRows;

/* Scale frame into the whole tiled scanout in one pass: the shown rectangle
 * of placement (pw_present_scale_placement for 1920x1080) nearest-neighbour,
 * the rest background (B,G,R), every pixel converted for the scanout. The
 * result is what pw_present_scale onto a linear 1920x1080 target followed
 * by a tiled copy produces, without the intermediate 8 MB image.
 *
 * A tile keeps each 4x4 block of pixels in 64 contiguous bytes, one cache
 * line, so four scanout rows are converted into rows first and then written
 * a block at a time; a row repeating the one above (vertical scaling) is
 * converted once. Source columns come from a table filled once per frame
 * and rows from one division each, so no pixel divides. placement must lie
 * inside the scanout and frame must be valid (pw_present_validate). */
static inline void pw_videoout_tiles_scale_to(const PwVideoOutTiles *tiles, PwVideoOutScaleRows *rows,
                                              const PwPresentFrame *frame,
                                              const PwPresentPlacement *placement, uint32_t background,
                                              int bgra, uint32_t *output)
{
    enum { W = PW_VIDEOOUT_TILE_WIDTH, H = PW_VIDEOOUT_TILE_HEIGHT };
    _Static_assert(W % 4 == 0 && H % 4 == 0, "4x4 blocks divide the scanout");
    const uint32_t left = placement->left, right = placement->left + placement->shown_width;
    const uint32_t top = placement->top, bottom = placement->top + placement->shown_height;
    const uint32_t fill = pw_videoout_scanout_pixel(background, bgra);
    uint32_t *const background_row = rows->row[4];

    /* floor(i * width / shown_width), stepped without dividing */
    for (uint32_t i = 0, column = 0, remainder = 0; i < placement->shown_width; i++) {
        rows->source_column[i] = column;
        remainder += frame->width;
        while (remainder >= placement->shown_width) {
            remainder -= placement->shown_width;
            column++;
        }
    }
    for (uint32_t x = 0; x < W; x++) background_row[x] = fill;
    for (uint32_t y = 0; y < H; y += 4) {
        const uint32_t *line[4];
        const uint8_t *previous = NULL;

        for (uint32_t j = 0; j < 4; j++) {
            if (y + j < top || y + j >= bottom) {
                line[j] = background_row;
                previous = NULL;
                continue;
            }
            const uint8_t *source_row = frame->pixels +
                (size_t)((uint64_t)(y + j - top) * frame->height / placement->shown_height) * frame->stride;
            if (source_row == previous) {
                line[j] = line[j - 1];
                continue;
            }
            const uint32_t *source = (const uint32_t *)(const void *)source_row;
            uint32_t *converted = rows->row[j];
            for (uint32_t x = 0; x < left; x++) converted[x] = fill;
            for (uint32_t x = left; x < right; x++)
                converted[x] = pw_videoout_scanout_pixel(source[rows->source_column[x - left]], bgra);
            for (uint32_t x = right; x < W; x++) converted[x] = fill;
            line[j] = converted;
            previous = source_row;
        }
        const uint32_t row_base = tiles->row_base[y], row_swizzle = tiles->row_swizzle[y];
        for (uint32_t x = 0; x < W; x += 4) {
            uint32_t *block = output + tiles->column_base[x] + row_base + (tiles->column_swizzle[x] ^ row_swizzle);
            memcpy(block, line[0] + x, 16);
            memcpy(block + 4, line[1] + x, 16);
            memcpy(block + 8, line[2] + x, 16);
            memcpy(block + 12, line[3] + x, 16);
        }
    }
}

/* pw_videoout_tiles_scale_to for the title's own scanout. */
static inline void pw_videoout_tiles_scale(const PwVideoOutTiles *tiles, PwVideoOutScaleRows *rows,
                                           const PwPresentFrame *frame,
                                           const PwPresentPlacement *placement, uint32_t background,
                                           uint32_t *output)
{
    pw_videoout_tiles_scale_to(tiles, rows, frame, placement, background, 0, output);
}
#endif
