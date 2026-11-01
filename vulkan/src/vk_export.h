#ifndef MBVK_EXPORT_H
#define MBVK_EXPORT_H

#include "vk_internal.h"

typedef struct export_rect {
    int plane;
    int source_x;
    int source_y;
    int target_x;
    int target_y;
    int width;
    int height;
} export_rect;

mb_status export_record_output(mbvk_backend *vk, const mb_output_copy *copy);
void exports_destroy(mbvk_backend *vk);

#endif
