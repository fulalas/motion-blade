#ifndef MB_CPU_RESOURCES_H
#define MB_CPU_RESOURCES_H

#include <stddef.h>
#include <stdint.h>

#include "common/texture.h"
#include "common/vec.h"
#include "motionblade/resources.h"

struct mb_image {
    ref_texture texture;
};

struct mb_buffer {
    size_t bytes;
    void *data;
};

static inline vec4 *vec4s(const mb_buffer *buffer)
{
    return (vec4 *)buffer->data;
}

static inline vec2 *vec2s(const mb_buffer *buffer)
{
    return (vec2 *)buffer->data;
}

static inline float *floats(const mb_buffer *buffer)
{
    return (float *)buffer->data;
}

static inline uint32_t *words(const mb_buffer *buffer)
{
    return (uint32_t *)buffer->data;
}

#endif
