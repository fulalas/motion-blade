#ifndef MOTIONBLADE_VIDEO_H
#define MOTIONBLADE_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mb_video_format {
    int width;
    int height;
    int bit_depth;
    int chroma_shift_x;
    int chroma_shift_y;
    bool interleaved_chroma;
    int lsb_padding;
} mb_video_format;

typedef struct mb_host_image {
    uint8_t *planes[3];
    ptrdiff_t strides[3];
} mb_host_image;

typedef struct mb_rect {
    int x;
    int y;
    int width;
    int height;
} mb_rect;

bool mb_video_format_is_valid(const mb_video_format *format);
int mb_plane_count(const mb_video_format *format);
int mb_bytes_per_sample(const mb_video_format *format);
int mb_chroma_width(const mb_video_format *format);
int mb_chroma_height(const mb_video_format *format);
int mb_shift_up(int value, int shift);
mb_rect mb_full_rect(const mb_video_format *format);
mb_rect mb_plane_area(const mb_video_format *format, int plane, mb_rect area);
bool mb_rect_equal(mb_rect a, mb_rect b);
bool mb_rect_contains(mb_rect outer, mb_rect inner);
mb_rect mb_rect_union(mb_rect a, mb_rect b);
mb_rect mb_rect_intersection(mb_rect a, mb_rect b);
size_t mb_plane_row_bytes(const mb_video_format *format, int plane);
int mb_plane_rows(const mb_video_format *format, int plane);
void mb_host_image_copy(const mb_host_image *target, const mb_host_image *source, const mb_video_format *format);
void mb_host_image_copy_outside(const mb_host_image *target, const mb_host_image *source,
                                const mb_video_format *format, mb_rect area);

#endif
