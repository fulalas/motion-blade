#ifndef EVAL_PIXEL_FORMATS_H
#define EVAL_PIXEL_FORMATS_H

#include <stdint.h>

#include "motionblade/video.h"

typedef struct test_format {
    const char *name;
    int bit_depth;
    bool interleaved;
    int lsb_padding;
    int bytes;
    int shift;
} test_format;

typedef struct converted_frame {
    uint8_t *storage;
    mb_host_image image;
    const test_format *format;
    int width;
    int height;
} converted_frame;

const test_format *test_format_find(const char *name);
const test_format *test_format_default(void);
void test_formats_list(void);
mb_video_format test_format_video(const test_format *format, int width, int height);
bool converted_alloc(converted_frame *frame, const test_format *format, int width, int height);
void converted_free(converted_frame *frame);
void convert_from_yuv420(converted_frame *frame, const uint8_t *source);
void convert_to_yuv420(const converted_frame *frame, uint8_t *target);
mb_host_image yuv420_image(uint8_t *frame, int width, int height);

#endif
