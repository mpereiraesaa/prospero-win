/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L    /* clock_gettime */
#include "../native/pw_videoout_tile.h"
#include "../native/pw_videoout_layout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    W = PW_VIDEOOUT_TILE_WIDTH, H = PW_VIDEOOUT_TILE_HEIGHT,
    /* the last row of tiles is partial, but its indices span a whole one */
    TILED = (H + 127) / 128 * 128 * W,
    SOURCE_MAX = 1920 * 1080 + 64,
};

static PwVideoOutTiles tiles;
static PwVideoOutScaleRows rows;
static uint8_t linear[W * H * 4];
static uint32_t two_pass[TILED], one_pass[TILED];
static uint32_t source[SOURCE_MAX];

/* The two passes Wine frames took: pw_present_scale onto a linear
 * 1920x1080 image, then pw_videoout_ps5_present's tiled copy of a view
 * that covers the scanout (placed at the origin, scale 1). */
static int scale_two_pass(const PwPresentFrame *frame, int mode, uint32_t background)
{
    const PwPresentTarget target = { linear, W, H, W * 4u, sizeof(linear) };
    PwVideoOutLayout layout;
    int status = pw_present_scale(frame, mode, background, &target, NULL);

    if (status != PW_OK) return status;
    assert(pw_videoout_layout(W, H, W, H, 80u, 3u, &layout) == PW_OK);
    assert(layout.scale == 1 && !layout.left && !layout.top && layout.source_width == W);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint32_t pixel;
            memcpy(&pixel, linear + ((size_t)y * W + x) * 4u, 4);
            two_pass[pw_videoout_tile_pixel(x, y)] = pw_videoout_rgbx(pixel);
        }
    return PW_OK;
}

static int scale_one_pass(const PwPresentFrame *frame, int mode, uint32_t background)
{
    PwPresentPlacement placement;
    int status = pw_present_scale_placement(frame, mode, W, H, &placement);

    if (status != PW_OK) return status;
    pw_videoout_tiles_scale(&tiles, &rows, frame, &placement, background, one_pass);
    return PW_OK;
}

static void fill_source(uint32_t seed)
{
    for (uint32_t i = 0; i < SOURCE_MAX; i++) source[i] = (i * 2654435761u) ^ seed;
}

/* One pass writes exactly the bytes of the two, for every mode. */
static void check_same(uint32_t width, uint32_t height, uint32_t stride_pixels, uint32_t background)
{
    const PwPresentFrame frame = { (const uint8_t *)source, width, height, stride_pixels * 4u,
                                   PW_PRESENT_BGRX8 };

    assert((uint64_t)stride_pixels * height <= SOURCE_MAX);
    for (int mode = PW_PRESENT_SCALE_FIT; mode <= PW_PRESENT_SCALE_STRETCH; mode++) {
        memset(two_pass, 0x5a, sizeof(two_pass));
        memset(one_pass, 0x5a, sizeof(one_pass));
        int status = scale_two_pass(&frame, mode, background);
        assert(scale_one_pass(&frame, mode, background) == status);
        if (status == PW_OK) assert(!memcmp(two_pass, one_pass, sizeof(one_pass)));
    }
}

static double cpu_milliseconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Host timing of an 800x600 game frame fitted to 1080p, the best of a few
 * rounds of this thread's CPU time, for the log only. */
static void benchmark(void)
{
    const PwPresentFrame frame = { (const uint8_t *)source, 800, 600, 800 * 4u, PW_PRESENT_BGRX8 };
    const PwPresentTarget target = { linear, W, H, W * 4u, sizeof(linear) };
    double best_two = 1e9, best_one = 1e9;

    for (int round = 0; round < 5; round++) {
        double t0 = cpu_milliseconds();
        assert(pw_present_scale(&frame, PW_PRESENT_SCALE_FIT, 0, &target, NULL) == PW_OK);
        for (uint32_t y = 0; y < H; y++) {
            const uint32_t *row = (const uint32_t *)(const void *)(linear + (size_t)y * W * 4u);
            for (uint32_t x = 0; x < W; x++)
                two_pass[pw_videoout_tiles_index(&tiles, x, y)] = pw_videoout_rgbx(row[x]);
        }
        double t1 = cpu_milliseconds();
        assert(scale_one_pass(&frame, PW_PRESENT_SCALE_FIT, 0) == PW_OK);
        double t2 = cpu_milliseconds();
        if (t1 - t0 < best_two) best_two = t1 - t0;
        if (t2 - t1 < best_one) best_one = t2 - t1;
    }
    printf("videoout scale, 800x600 fit to 1920x1080, host CPU: two passes %.2f ms, one pass %.2f ms\n",
           best_two, best_one);
}

int main(void)
{
    pw_videoout_tiles_init(&tiles);
    /* The tables give every pixel the index of the direct formula. */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            assert(pw_videoout_tiles_index(&tiles, x, y) == pw_videoout_tile_pixel(x, y));
            assert(pw_videoout_tile_pixel(x, y) < TILED);
        }
    assert(pw_videoout_tile_pixel(0, 0) == 0 && pw_videoout_tile_pixel(1, 0) == 1);
    assert(pw_videoout_tile_pixel(0, 1) == 4 && pw_videoout_tile_pixel(512, 0) == 512u * 128u);

    /* Red and blue swap, green stays, the X byte becomes opaque. */
    assert(pw_videoout_rgbx(0x00112233u) == 0xff332211u && pw_videoout_rgbx(0xabffffffu) == 0xffffffffu);

    fill_source(0x9e3779b9u);
    check_same(800, 600, 800, 0x000000u);    /* a Wine desktop: pillarboxed 1440x1080 */
    check_same(640, 480, 700, 0x102030u);    /* padded rows, a coloured background */
    check_same(1920, 1080, 1920, 0);         /* covers the screen exactly */
    check_same(1024, 768, 1024, 0x0000ffu);  /* integer mode leaves it at 1x, centred */
    check_same(160, 600, 160, 0x404040u);    /* a tall window view */
    check_same(1920, 200, 1920, 0);          /* a wide strip, letterboxed */
    check_same(7, 5, 9, 0x123456u);          /* awkward ratios */
    check_same(1, 1, 1, 0);
    check_same(1, 1080, 1, 0);
    check_same(1921, 10, 1921, 0);           /* wider than the screen: integer refuses */
    fill_source(0x1234567u);
    check_same(800, 600, 800, 0xffffffu);

    benchmark();
    printf("videoout tiles passed: table indices equal the tiled formula for all %u pixels; "
           "scaling straight into the tiles writes the bytes of the two-pass path\n", W * H);
    return 0;
}
