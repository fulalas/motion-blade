#include "pixel_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const test_format FORMATS[] = {
    {"yuv420p", 8, false, 0, 1, 0},
    {"nv12", 8, true, 0, 1, 0},
    {"p010", 16, true, 6, 2, 8},
    {"yuv420p10", 10, false, 0, 2, 2},
    {"p016", 16, true, 0, 2, 0},
};

enum { FORMAT_COUNT = sizeof(FORMATS) / sizeof(FORMATS[0]) };

const test_format *test_format_find(const char *name)
{
    for (int i = 0; i < FORMAT_COUNT; i++) {
        if (!strcmp(FORMATS[i].name, name)) {
            return &FORMATS[i];
        }
    }
    return NULL;
}

const test_format *test_format_default(void)
{
    return &FORMATS[0];
}

void test_formats_list(void)
{
    for (int i = 0; i < FORMAT_COUNT; i++) {
        fprintf(stderr, "%s%s", i ? "|" : "", FORMATS[i].name);
    }
}

mb_video_format test_format_video(const test_format *format, int width, int height)
{
    return (mb_video_format){width, height, format->bit_depth, 1, 1, format->interleaved, format->lsb_padding};
}

mb_host_image yuv420_image(uint8_t *frame, int width, int height)
{
    size_t luma = (size_t)width * (size_t)height;
    return (mb_host_image){{frame, frame + luma, frame + luma + luma / 4}, {width, width / 2, width / 2}};
}

bool converted_alloc(converted_frame *frame, const test_format *format, int width, int height)
{
    size_t luma = (size_t)width * (size_t)height * (size_t)format->bytes;
    size_t chroma = (size_t)(width / 2) * (size_t)(height / 2) * (size_t)format->bytes;
    frame->storage = calloc(1, luma + 2 * chroma);
    if (!frame->storage) {
        return false;
    }
    frame->format = format;
    frame->width = width;
    frame->height = height;
    mb_host_image *im = &frame->image;
    *im = (mb_host_image){{frame->storage, frame->storage + luma, NULL}, {(ptrdiff_t)width * format->bytes, 0, 0}};
    if (format->interleaved) {
        im->strides[1] = (ptrdiff_t)width * format->bytes;
    } else {
        im->planes[2] = frame->storage + luma + chroma;
        im->strides[1] = im->strides[2] = (ptrdiff_t)(width / 2) * format->bytes;
    }
    return true;
}

void converted_free(converted_frame *frame)
{
    free(frame->storage);
    frame->storage = NULL;
}

static void write_sample(uint8_t *p, int value, const test_format *format)
{
    if (format->bytes == 1) {
        *p = (uint8_t)value;
        return;
    }
    uint16_t wide = (uint16_t)(format->shift ? value << format->shift : value * 257);
    memcpy(p, &wide, sizeof(wide));
}

static int read_sample(const uint8_t *p, const test_format *format)
{
    if (format->bytes == 1) {
        return *p;
    }
    uint16_t wide;
    memcpy(&wide, p, sizeof(wide));
    int value = format->shift ? (wide + (1 << (format->shift - 1))) >> format->shift : (wide + 128) / 257;
    return value > 255 ? 255 : value;
}

static uint8_t *sample_at(const converted_frame *frame, int plane, int x, int y)
{
    const mb_host_image *im = &frame->image;
    return im->planes[plane] + (ptrdiff_t)y * im->strides[plane] + (ptrdiff_t)x * frame->format->bytes;
}

void convert_from_yuv420(converted_frame *frame, const uint8_t *source)
{
    const test_format *f = frame->format;
    int w = frame->width;
    mb_host_image in = yuv420_image((uint8_t *)source, w, frame->height);
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < w; x++) {
            write_sample(sample_at(frame, 0, x, y), in.planes[0][(size_t)y * (size_t)w + (size_t)x], f);
        }
    }
    for (int y = 0; y < frame->height / 2; y++) {
        for (int x = 0; x < w / 2; x++) {
            size_t i = (size_t)y * (size_t)(w / 2) + (size_t)x;
            write_sample(sample_at(frame, 1, f->interleaved ? 2 * x : x, y), in.planes[1][i], f);
            uint8_t *vp = f->interleaved ? sample_at(frame, 1, 2 * x + 1, y) : sample_at(frame, 2, x, y);
            write_sample(vp, in.planes[2][i], f);
        }
    }
}

void convert_to_yuv420(const converted_frame *frame, uint8_t *target)
{
    const test_format *f = frame->format;
    int w = frame->width;
    mb_host_image out = yuv420_image(target, w, frame->height);
    for (int y = 0; y < frame->height; y++) {
        for (int x = 0; x < w; x++) {
            out.planes[0][(size_t)y * (size_t)w + (size_t)x] = (uint8_t)read_sample(sample_at(frame, 0, x, y), f);
        }
    }
    for (int y = 0; y < frame->height / 2; y++) {
        for (int x = 0; x < w / 2; x++) {
            size_t i = (size_t)y * (size_t)(w / 2) + (size_t)x;
            out.planes[1][i] = (uint8_t)read_sample(sample_at(frame, 1, f->interleaved ? 2 * x : x, y), f);
            uint8_t *vp = f->interleaved ? sample_at(frame, 1, 2 * x + 1, y) : sample_at(frame, 2, x, y);
            out.planes[2][i] = (uint8_t)read_sample(vp, f);
        }
    }
}
