#ifndef MBVK_PIPELINES_H
#define MBVK_PIPELINES_H

#include "vk_internal.h"

enum { PUSH_CONSTANT_BYTES = 128 };

typedef struct binding {
    const struct mb_image *image;
    const struct mb_buffer *buffer;
} binding;

typedef struct push_data {
    const void *data;
    size_t bytes;
} push_data;

#define BINDINGS(array) (array), (int)(sizeof(array) / sizeof((array)[0]))
#define PUSH(constants) ((push_data){&(constants), sizeof(constants)})
#define PUSH_FITS(type) _Static_assert(sizeof(type) <= PUSH_CONSTANT_BYTES, #type " exceeds the push constant range")

typedef struct dispatch_desc {
    pass_id pass;
    const binding *bindings;
    int binding_count;
    push_data push;
    const char *name;
    uint32_t groups_x;
    uint32_t groups_y;
} dispatch_desc;

mb_status pipelines_create(mbvk_backend *vk);
void pipelines_destroy(mbvk_backend *vk);
void pipeline_dispatch(mbvk_backend *vk, const dispatch_desc *desc);
uint32_t pipeline_groups(pass_id pass, int items);
pass_id warp_variant(pass_id first, bool interleaved, bool sixteen_bit);
void pipelines_prepare(mbvk_backend *vk, const mb_video_format *format);

#endif
