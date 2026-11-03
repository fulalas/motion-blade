#include "compensation/chroma.h"

#include "compensation/cubic.h"
#include "reference/cpu_resources.h"

chroma_channels chroma_channels_of(const mb_frame_planes *planes, const mb_chroma_layout *chroma)
{
    ref_texture *first = &planes->chroma[0]->texture;
    if (chroma->interleaved) {
        return (chroma_channels){first, first, 1};
    }
    return (chroma_channels){first, &planes->chroma[1]->texture, 0};
}

vec2 chroma_from_luma(const mb_chroma_layout *chroma, vec2 luma)
{
    return v2(luma.x / (float)(1 << chroma->shift_x), luma.y / (float)(1 << chroma->shift_y));
}

vec2 chroma_sample(const mb_frame_planes *planes, const mb_chroma_layout *chroma, vec2 position)
{
    vec2 inv = v2_inverse_size(chroma->width, chroma->height);
    vec2 uv = v2(position.x * inv.x, position.y * inv.y);
    chroma_channels c = chroma_channels_of(planes, chroma);
    return v2(ref_sample(c.first, uv.x, uv.y, 0), ref_sample(c.second, uv.x, uv.y, c.second_channel));
}

vec2 chroma_cubic_sample(const mb_frame_planes *planes, const mb_chroma_layout *chroma, vec2 position)
{
    chroma_channels c = chroma_channels_of(planes, chroma);
    return v2(cubic_sample(c.first, position, 0), cubic_sample(c.second, position, c.second_channel));
}
