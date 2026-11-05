#include "frames.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pixel_formats.h"
#include "samples.h"

bool test_frame_alloc(test_frame *frame, int width, int height)
{
    frame->format = test_format_video(test_format_default(), width, height);
    frame->storage = calloc(1, (size_t)width * (size_t)height * 3 / 2);
    if (!frame->storage) {
        return false;
    }
    frame->image = yuv420_image(frame->storage, width, height);
    return true;
}

void test_frame_free(test_frame *frame)
{
    free(frame->storage);
    frame->storage = NULL;
}

static unsigned scramble(unsigned x)
{
    x ^= x >> 13;
    x *= 0x5bd1e995u;
    x ^= x >> 15;
    return x;
}

static double texture_value(int x, int y, int seed)
{
    double smooth = 60.0 * sin(0.23 * x + seed) * cos(0.19 * y - seed) + 30.0 * sin(0.07 * (x + y));
    unsigned cell = scramble((unsigned)(x >> 3) * 73856093u ^ (unsigned)(y >> 3) * 19349663u ^ (unsigned)seed);
    return 128.0 + smooth + (double)(cell % 40) - 20.0;
}

void paint_texture(test_frame *frame, test_motion motion)
{
    int w = frame->format.width;
    int h = frame->format.height;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            frame->image.planes[0][y * w + x] = clamp_byte(texture_value(x - motion.dx, y - motion.dy, motion.seed));
        }
    }
    for (int y = 0; y < h / 2; y++) {
        for (int x = 0; x < w / 2; x++) {
            double luma = texture_value(2 * x - motion.dx, 2 * y - motion.dy, motion.seed);
            frame->image.planes[1][y * (w / 2) + x] = clamp_byte(128 + (luma - 128) * 0.2);
            frame->image.planes[2][y * (w / 2) + x] = clamp_byte(128 - (luma - 128) * 0.15);
        }
    }
}

void paint_bars(test_frame *frame, int rows)
{
    int w = frame->format.width;
    int h = frame->format.height;
    for (int y = 0; y < h; y++) {
        if (y >= rows && y < h - rows) {
            continue;
        }
        memset(frame->image.planes[0] + y * w, 16, (size_t)w);
        memset(frame->image.planes[1] + (y / 2) * (w / 2), 128, (size_t)(w / 2));
        memset(frame->image.planes[2] + (y / 2) * (w / 2), 128, (size_t)(w / 2));
    }
}

double luma_psnr(const test_frame *a, const test_frame *b, mb_rect area)
{
    double squared = 0;
    int w = a->format.width;
    for (int y = area.y; y < area.y + area.height; y++) {
        for (int x = area.x; x < area.x + area.width; x++) {
            int d = a->image.planes[0][y * w + x] - b->image.planes[0][y * w + x];
            squared += d * d;
        }
    }
    return psnr_decibels(squared / ((double)area.width * area.height));
}

bool luma_rows_equal(const test_frame *a, const test_frame *b, int first_row, int rows)
{
    int w = a->format.width;
    return memcmp(a->image.planes[0] + first_row * w, b->image.planes[0] + first_row * w, (size_t)(w * rows)) == 0;
}

enum { SCENE_CUT_FRAME = 8, LIGHT_FRAME = 5, FIRST_OF_CUTS = 6, BOX_SIZE = 48 };

static void paint_box(test_frame *frame, int x0, int y0, int seed)
{
    int w = frame->format.width;
    int h = frame->format.height;
    for (int y = y0; y < y0 + BOX_SIZE && y < h; y++) {
        for (int x = x0; x < x0 + BOX_SIZE && x < w; x++) {
            if (x >= 0 && y >= 0) {
                frame->image.planes[0][y * w + x] = clamp_byte(texture_value(x - x0, y - y0, seed) * 0.8 + 40);
            }
        }
    }
}

static void brighten(test_frame *frame, int amount)
{
    size_t count = (size_t)frame->format.width * (size_t)frame->format.height;
    for (size_t i = 0; i < count; i++) {
        frame->image.planes[0][i] = clamp_byte(frame->image.planes[0][i] + amount);
    }
}

void paint_scene(test_frame *frame, int index)
{
    int seed = index >= SCENE_CUT_FRAME ? 31 : 11;
    paint_texture(frame, (test_motion){3 * index, index, seed});
    paint_box(frame, frame->format.width - 60 - 5 * index, 40 + index, seed + 1);
    if (index == LIGHT_FRAME) {
        brighten(frame, 20);
    }
}

void paint_cuts_in_a_row(test_frame *frame, int index)
{
    static const int SEEDS[] = {11, 31, 47, 59};
    int shot = index < FIRST_OF_CUTS ? 0 : index - FIRST_OF_CUTS + 1;
    int seed = SEEDS[shot < 3 ? shot : 3];
    paint_texture(frame, (test_motion){3 * index, index, seed});
    paint_box(frame, frame->format.width - 60 - 5 * index, 40 + index, seed + 1);
}
