#include "framing/bars.h"

#include <string.h>

#include "motionblade/layout.h"

enum { SCAN_STEP = 2, RECHECK_STEP = 3, ALIGN = 8 };

static const double MIN_CROPPED_SHARE = 0.96;

typedef struct black_levels {
    unsigned luma_max;
    unsigned chroma_lo;
    unsigned chroma_hi;
} black_levels;

static black_levels levels_of(const mb_video_format *format)
{
    unsigned mid = 1u << (format->bit_depth - 1);
    unsigned scale = mid / 128;
    return (black_levels){MB_BLACK_LUMA_MAX * scale, mid - MB_BLACK_CHROMA_TOLERANCE * scale,
                          mid + MB_BLACK_CHROMA_TOLERANCE * scale};
}

static unsigned sample_at(const bar_source *source, int plane, int x, int y)
{
    const uint8_t *row = source->image->planes[plane] + (ptrdiff_t)y * source->image->strides[plane];
    if (mb_bytes_per_sample(source->format) == 1) {
        return row[x];
    }
    uint16_t value;
    memcpy(&value, row + (size_t)x * 2, sizeof(value));
    return value;
}

static bool chroma_is_neutral(const bar_source *source, black_levels levels, int x, int y)
{
    const mb_video_format *f = source->format;
    int cx = x >> f->chroma_shift_x;
    int cy = y >> f->chroma_shift_y;
    for (int k = 0; k < 2; k++) {
        unsigned v = f->interleaved_chroma ? sample_at(source, 1, cx * 2 + k, cy) : sample_at(source, 1 + k, cx, cy);
        if (v < levels.chroma_lo || v > levels.chroma_hi) {
            return false;
        }
    }
    return true;
}

static bool span_is_black(const bar_source *source, int y, int x0, int x1)
{
    black_levels levels = levels_of(source->format);
    for (int x = x0; x < x1; x += SCAN_STEP) {
        if (sample_at(source, 0, x, y) > levels.luma_max || !chroma_is_neutral(source, levels, x, y)) {
            return false;
        }
    }
    return true;
}

static bool row_is_black(const bar_source *source, int y)
{
    if (source->flags) {
        return source->flags[y] != 0;
    }
    return span_is_black(source, y, 0, source->format->width);
}

static bool column_is_black(const bar_source *source, int x, int y0, int y1)
{
    if (source->flags) {
        return source->flags[source->format->height + x] != 0;
    }
    for (int y = y0; y < y1; y += SCAN_STEP) {
        if (!span_is_black(source, y, x, x + 1)) {
            return false;
        }
    }
    return true;
}

static int align_down(int value)
{
    return value & ~(ALIGN - 1);
}

static int align_up(int value, int limit)
{
    int aligned = (value + ALIGN - 1) & ~(ALIGN - 1);
    return aligned < limit ? aligned : limit;
}

typedef struct edges {
    int top;
    int bottom;
    int left;
    int right;
} edges;

static edges find_edges(const bar_source *source)
{
    int w = source->format->width;
    int h = source->format->height;
    edges e = {0, h, 0, w};
    while (e.top < h / 4 && row_is_black(source, e.top)) {
        e.top++;
    }
    while (e.bottom > h - h / 4 && row_is_black(source, e.bottom - 1)) {
        e.bottom--;
    }
    while (e.left < w / 4 && column_is_black(source, e.left, e.top, e.bottom)) {
        e.left++;
    }
    while (e.right > w - w / 4 && column_is_black(source, e.right - 1, e.top, e.bottom)) {
        e.right--;
    }
    return e;
}

bool bars_detect(const bar_source *source, mb_rect *active)
{
    int w = source->format->width;
    int h = source->format->height;
    edges e = find_edges(source);
    if (e.top == h / 4 || e.bottom == h - h / 4 || e.left == w / 4 || e.right == w - w / 4) {
        return false;
    }
    int x0 = align_down(e.left);
    int y0 = align_down(e.top);
    int x1 = align_up(e.right, w);
    int y1 = align_up(e.bottom, h);
    bool odd = ((x1 - x0) & 1) || ((y1 - y0) & 1);
    if (odd || (double)(x1 - x0) * (y1 - y0) > MIN_CROPPED_SHARE * w * h) {
        *active = (mb_rect){0, 0, w, h};
    } else {
        *active = (mb_rect){x0, y0, x1 - x0, y1 - y0};
    }
    return true;
}

static bool sides_are_black(const bar_source *source, int y, mb_rect active)
{
    int x1 = active.x + active.width;
    return span_is_black(source, y, 0, active.x) && span_is_black(source, y, x1, source->format->width);
}

static bool rows_still_black(const bar_source *source, mb_rect active)
{
    int y1 = active.y + active.height;
    for (int y = 0; y < source->format->height; y += RECHECK_STEP) {
        bool in_bar = y < active.y || y >= y1;
        if (in_bar && !row_is_black(source, y)) {
            return false;
        }
        if (!in_bar && source->image && !sides_are_black(source, y, active)) {
            return false;
        }
    }
    return true;
}

static bool columns_still_black(const bar_source *source, mb_rect active)
{
    int x1 = active.x + active.width;
    for (int x = 0; source->flags && x < source->format->width; x += RECHECK_STEP) {
        bool in_bar = x < active.x || x >= x1;
        if (in_bar && !column_is_black(source, x, active.y, active.y + active.height)) {
            return false;
        }
    }
    return true;
}

bool bars_still_black(const bar_source *source, mb_rect active)
{
    return rows_still_black(source, active) && columns_still_black(source, active);
}
