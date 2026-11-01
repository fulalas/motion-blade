#ifndef MB_BORDER_H
#define MB_BORDER_H

#include <stdint.h>

#include "motionblade/video.h"

typedef struct border_template {
    mb_host_image image;
    uint8_t *storage;
} border_template;

bool border_create(border_template *border, const mb_video_format *format);
void border_destroy(border_template *border);
bool border_exists(const border_template *border);

#endif
