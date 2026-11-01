#include "common/texture.h"

#include "common/vec.h"
#include "motionblade/half.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SUBTEXEL_STEPS = 256 };

bool ref_texture_alloc(ref_texture *texture, const mb_image_desc *desc)
{
    int channels = mb_texel_channels(desc->format);
    size_t count = (size_t)desc->width * (size_t)desc->height * (size_t)channels;
    float *texels = calloc(count ? count : 1, sizeof(float));
    *texture = (ref_texture){desc->width, desc->height, channels, desc->format, texels};
    return texture->texels != NULL;
}

void ref_texture_free(ref_texture *texture)
{
    free(texture->texels);
    memset(texture, 0, sizeof(*texture));
}

float ref_texel(const ref_texture *texture, int x, int y, int channel)
{
    size_t row = (size_t)clampi(y, 0, texture->height - 1) * (size_t)texture->width;
    size_t index = (row + (size_t)clampi(x, 0, texture->width - 1)) * (size_t)texture->channels;
    return texture->texels[index + (size_t)channel];
}

void ref_store(ref_texture *texture, int x, int y, int channel, float value)
{
    size_t index = ((size_t)y * (size_t)texture->width + (size_t)x) * (size_t)texture->channels;
    texture->texels[index + (size_t)channel] = ref_quantize(texture->format, value);
}

static float unorm(float value, float levels)
{
    return rintf(clampf(value, 0.0f, 1.0f) * levels) / levels;
}

float ref_quantize(mb_texel_format format, float value)
{
    switch (format) {
    case MB_TEXELS_R8:
    case MB_TEXELS_RG8:
        return unorm(value, 255.0f);
    case MB_TEXELS_R16:
    case MB_TEXELS_RG16:
        return unorm(value, 65535.0f);
    case MB_TEXELS_R16F:
        return ref_half(value);
    }
    return value;
}

static float subtexel(float fraction)
{
    return rintf(fraction * SUBTEXEL_STEPS) / SUBTEXEL_STEPS;
}

float ref_sample(const ref_texture *texture, float u, float v, int channel)
{
    float x = u * (float)texture->width - 0.5f;
    float y = v * (float)texture->height - 0.5f;
    float x0 = floorf(x);
    float y0 = floorf(y);
    float wx = subtexel(x - x0);
    float wy = subtexel(y - y0);
    int ix = (int)x0;
    int iy = (int)y0;
    float top = mixf(ref_texel(texture, ix, iy, channel), ref_texel(texture, ix + 1, iy, channel), wx);
    float bottom = mixf(ref_texel(texture, ix, iy + 1, channel), ref_texel(texture, ix + 1, iy + 1, channel), wx);
    return mixf(top, bottom, wy);
}

float ref_half(float value)
{
    return mb_half_to_float(mb_half_from_float(value));
}
