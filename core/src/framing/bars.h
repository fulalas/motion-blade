#ifndef MB_BARS_H
#define MB_BARS_H

#include <stdint.h>

#include "motionblade/video.h"

typedef struct bar_source {
    const mb_video_format *format;
    const mb_host_image *image;
    const uint32_t *flags;
} bar_source;

bool bars_detect(const bar_source *source, mb_rect *active);
bool bars_still_black(const bar_source *source, mb_rect active);

#endif
