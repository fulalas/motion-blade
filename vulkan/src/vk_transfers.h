#ifndef MBVK_TRANSFERS_H
#define MBVK_TRANSFERS_H

#include "vk_internal.h"

typedef struct image_region {
    struct mb_image *source;
    int source_x;
    int source_y;
    struct mb_image *target;
    int target_x;
    int target_y;
    int width;
    int height;
} image_region;

typedef struct bar_rects {
    mb_rect rects[4];
    int count;
} bar_rects;

bar_rects bars_around(mb_rect inner, const struct mb_image *plane);
bool area_is_cropped(mb_rect area, const mb_video_format *format);
void copy_image_region(mbvk_backend *vk, const image_region *region);
void copy_overlap_images(mbvk_backend *vk, const mb_overlap_copy *copy);
void capture_stats(mbvk_backend *vk, mb_buffer *stats);
void remember_output(job_slot *s, const mb_output_copy *copy, output_target target);

void vk_fill_buffer(void *self, mb_buffer *buffer, uint32_t value);
void vk_copy_overlap(void *self, const mb_overlap_copy *copy);
mb_status vk_upload_frame(void *self, const mb_upload *upload);
mb_status vk_record_host_output(void *self, const mb_output_copy *copy, mb_buffer *stats);
bool vk_output_ready(void *self, mb_ticket ticket);
mb_status vk_read_output(void *self, mb_ticket ticket, const mb_host_image *target, mb_rect area);
mb_status vk_finish_output(void *self, mb_ticket ticket, mb_output_stats *stats);
mb_status vk_read_buffer(void *self, mb_buffer *buffer, void *data, size_t bytes);
mb_status vk_write_buffer(void *self, mb_buffer *buffer, const void *data, size_t bytes);
mb_status vk_read_image(void *self, mb_image *image, float *texels);
mb_status vk_write_image(void *self, mb_image *image, const float *texels);

#endif
