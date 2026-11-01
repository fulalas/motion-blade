#ifndef MBVK_DEVICE_FRAMES_H
#define MBVK_DEVICE_FRAMES_H

#include "vk_internal.h"

mb_status vk_load_device_frame(void *self, const mb_device_load *load);
void vk_fill_from_border(void *self, mb_frame_planes planes, mb_rect area, const mb_video_format *format);
bool vk_poll_bar_flags(void *self, const uint32_t **flags);
void vk_clear_bar_flags(void *self);
mb_status vk_record_device_output(void *self, const mb_output_copy *copy, mb_buffer *stats);
void background_destroy(mbvk_backend *vk);

#endif
