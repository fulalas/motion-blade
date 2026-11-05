#ifndef MB_TEST_FRAMES_H
#define MB_TEST_FRAMES_H

#include <stdint.h>

#include "motionblade/video.h"

typedef struct test_frame {
    mb_video_format format;
    uint8_t *storage;
    mb_host_image image;
} test_frame;

typedef struct test_motion {
    int dx;
    int dy;
    int seed;
} test_motion;

bool test_frame_alloc(test_frame *frame, int width, int height);
void test_frame_free(test_frame *frame);
void paint_texture(test_frame *frame, test_motion motion);
void paint_bars(test_frame *frame, int rows);
void paint_scene(test_frame *frame, int index);
void paint_cuts_in_a_row(test_frame *frame, int index);
double luma_psnr(const test_frame *a, const test_frame *b, mb_rect area);
bool luma_rows_equal(const test_frame *a, const test_frame *b, int first_row, int rows);

#endif
