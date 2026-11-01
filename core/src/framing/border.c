#include "framing/border.h"

#include <stdlib.h>
#include <string.h>

static void fill_neutral(const mb_host_image *image, const mb_video_format *format)
{
    uint16_t mid = (uint16_t)(1u << (format->bit_depth - 1));
    int bytes = mb_bytes_per_sample(format);
    for (int plane = 1; plane < mb_plane_count(format); plane++) {
        for (int y = 0; y < mb_plane_rows(format, plane); y++) {
            uint8_t *row = image->planes[plane] + (ptrdiff_t)y * image->strides[plane];
            for (size_t x = 0; x < mb_plane_row_bytes(format, plane); x += (size_t)bytes) {
                if (bytes == 1) {
                    row[x] = (uint8_t)mid;
                } else {
                    memcpy(row + x, &mid, sizeof(mid));
                }
            }
        }
    }
}

bool border_create(border_template *border, const mb_video_format *format)
{
    size_t total = 0;
    size_t offsets[3] = {0, 0, 0};
    for (int plane = 0; plane < mb_plane_count(format); plane++) {
        offsets[plane] = total;
        total += mb_plane_row_bytes(format, plane) * (size_t)mb_plane_rows(format, plane);
    }
    border->storage = calloc(1, total ? total : 1);
    if (!border->storage) {
        return false;
    }
    for (int plane = 0; plane < mb_plane_count(format); plane++) {
        border->image.planes[plane] = border->storage + offsets[plane];
        border->image.strides[plane] = (ptrdiff_t)mb_plane_row_bytes(format, plane);
    }
    fill_neutral(&border->image, format);
    return true;
}

void border_destroy(border_template *border)
{
    free(border->storage);
    memset(border, 0, sizeof(*border));
}

bool border_exists(const border_template *border)
{
    return border->storage != NULL;
}
