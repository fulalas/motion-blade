#ifndef MB_TEXTURE_H
#define MB_TEXTURE_H

#include <stdbool.h>

#include "motionblade/resources.h"

typedef struct ref_texture {
    int width;
    int height;
    int channels;
    mb_texel_format format;
    float *texels;
} ref_texture;

bool ref_texture_alloc(ref_texture *texture, const mb_image_desc *desc);
void ref_texture_free(ref_texture *texture);
float ref_texel(const ref_texture *texture, int x, int y, int channel);
void ref_store(ref_texture *texture, int x, int y, int channel, float value);
float ref_quantize(mb_texel_format format, float value);
float ref_sample(const ref_texture *texture, float u, float v, int channel);
float ref_half(float value);

#endif
