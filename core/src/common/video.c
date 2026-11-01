#include "motionblade/video.h"

#include "motionblade/layout.h"

static int max_int(int a, int b)
{
    return a > b ? a : b;
}

static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static bool in_range(int value, int lo, int hi)
{
    return value >= lo && value <= hi;
}

bool mb_video_format_is_valid(const mb_video_format *format)
{
    bool padded_ok = format->lsb_padding == 0 || format->bit_depth == 16;
    return format->width >= MB_MIN_FRAME_SIZE && format->height >= MB_MIN_FRAME_SIZE &&
           in_range(format->bit_depth, 8, 16) && in_range(format->chroma_shift_x, 0, 1) &&
           in_range(format->chroma_shift_y, 0, 1) && in_range(format->lsb_padding, 0, 8) && padded_ok;
}

int mb_plane_count(const mb_video_format *format)
{
    return format->interleaved_chroma ? 2 : 3;
}

int mb_bytes_per_sample(const mb_video_format *format)
{
    return format->bit_depth > 8 ? 2 : 1;
}

int mb_chroma_width(const mb_video_format *format)
{
    return (format->width + (1 << format->chroma_shift_x) - 1) >> format->chroma_shift_x;
}

int mb_chroma_height(const mb_video_format *format)
{
    return (format->height + (1 << format->chroma_shift_y) - 1) >> format->chroma_shift_y;
}

bool mb_rect_equal(mb_rect a, mb_rect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool mb_rect_contains(mb_rect outer, mb_rect inner)
{
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

mb_rect mb_rect_union(mb_rect a, mb_rect b)
{
    int x0 = min_int(a.x, b.x);
    int y0 = min_int(a.y, b.y);
    int x1 = max_int(a.x + a.width, b.x + b.width);
    int y1 = max_int(a.y + a.height, b.y + b.height);
    return (mb_rect){x0, y0, x1 - x0, y1 - y0};
}

mb_rect mb_rect_intersection(mb_rect a, mb_rect b)
{
    int x0 = max_int(a.x, b.x);
    int y0 = max_int(a.y, b.y);
    int x1 = min_int(a.x + a.width, b.x + b.width);
    int y1 = min_int(a.y + a.height, b.y + b.height);
    return (mb_rect){x0, y0, max_int(x1 - x0, 0), max_int(y1 - y0, 0)};
}
