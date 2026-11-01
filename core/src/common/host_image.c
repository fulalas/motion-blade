#include <string.h>

#include "motionblade/video.h"

typedef struct plane_box {
    size_t x_bytes;
    size_t width_bytes;
    int y;
    int height;
} plane_box;

typedef struct row_span {
    int plane;
    int y;
    size_t x;
    size_t bytes;
} row_span;

static int divide_up(int value, int shift)
{
    return (value + (1 << shift) - 1) >> shift;
}

static size_t sample_bytes(const mb_video_format *format, int plane)
{
    int components = plane > 0 && format->interleaved_chroma ? 2 : 1;
    return (size_t)(components * mb_bytes_per_sample(format));
}

static plane_box box_of(const mb_video_format *format, int plane, mb_rect area)
{
    int sx = plane ? format->chroma_shift_x : 0;
    int sy = plane ? format->chroma_shift_y : 0;
    size_t sample = sample_bytes(format, plane);
    return (plane_box){(size_t)(area.x >> sx) * sample, (size_t)divide_up(area.width, sx) * sample, area.y >> sy,
                       divide_up(area.height, sy)};
}

static plane_box whole_plane(const mb_video_format *format, int plane)
{
    return box_of(format, plane, (mb_rect){0, 0, format->width, format->height});
}

size_t mb_plane_row_bytes(const mb_video_format *format, int plane)
{
    return whole_plane(format, plane).width_bytes;
}

int mb_plane_rows(const mb_video_format *format, int plane)
{
    return whole_plane(format, plane).height;
}

static void copy_span(const mb_host_image *target, const mb_host_image *source, row_span span)
{
    uint8_t *to = target->planes[span.plane] + (ptrdiff_t)span.y * target->strides[span.plane] + span.x;
    const uint8_t *from = source->planes[span.plane] + (ptrdiff_t)span.y * source->strides[span.plane] + span.x;
    memcpy(to, from, span.bytes);
}

void mb_host_image_copy(const mb_host_image *target, const mb_host_image *source, const mb_video_format *format)
{
    for (int plane = 0; plane < mb_plane_count(format); plane++) {
        plane_box whole = whole_plane(format, plane);
        for (int y = 0; y < whole.height; y++) {
            copy_span(target, source, (row_span){plane, y, 0, whole.width_bytes});
        }
    }
}

void mb_host_image_copy_outside(const mb_host_image *target, const mb_host_image *source,
                                const mb_video_format *format, mb_rect area)
{
    for (int plane = 0; plane < mb_plane_count(format); plane++) {
        plane_box whole = whole_plane(format, plane);
        plane_box inner = box_of(format, plane, area);
        size_t right = inner.x_bytes + inner.width_bytes;
        for (int y = 0; y < whole.height; y++) {
            if (y < inner.y || y >= inner.y + inner.height) {
                copy_span(target, source, (row_span){plane, y, 0, whole.width_bytes});
                continue;
            }
            copy_span(target, source, (row_span){plane, y, 0, inner.x_bytes});
            copy_span(target, source, (row_span){plane, y, right, whole.width_bytes - right});
        }
    }
}
