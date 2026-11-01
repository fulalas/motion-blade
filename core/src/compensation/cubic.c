#include "compensation/cubic.h"

static vec4 catmull_rom(float f)
{
    return (vec4){
        f * (-0.5f + f * (1.0f - 0.5f * f)),
        1.0f + f * f * (-2.5f + 1.5f * f),
        f * (0.5f + f * (2.0f - 1.5f * f)),
        f * f * (-0.5f + 0.5f * f),
    };
}

typedef struct cubic_source {
    const ref_texture *texture;
    int channel;
} cubic_source;

static float dot4(vec4 w, vec4 v)
{
    return w.x * v.x + w.y * v.y + w.z * v.z + w.w * v.w;
}

static float texel(cubic_source s, int x, int y)
{
    return ref_texel(s.texture, x, y, s.channel);
}

static float filtered_row(cubic_source s, vec4 wx, ivec2 at)
{
    vec4 row = {texel(s, at.x - 1, at.y), texel(s, at.x, at.y), texel(s, at.x + 1, at.y), texel(s, at.x + 2, at.y)};
    return dot4(wx, row);
}

static float nearest_clamp(cubic_source s, ivec2 at, float value)
{
    float a = texel(s, at.x, at.y);
    float b = texel(s, at.x + 1, at.y);
    float c = texel(s, at.x, at.y + 1);
    float d = texel(s, at.x + 1, at.y + 1);
    float lo = fminf(fminf(a, b), fminf(c, d));
    float hi = fmaxf(fmaxf(a, b), fmaxf(c, d));
    return clampf(value, lo, hi);
}

float cubic_sample(const ref_texture *texture, vec2 position, int channel)
{
    float kx = floorf(position.x - 0.5f);
    float ky = floorf(position.y - 0.5f);
    vec2 f = v2(position.x - 0.5f - kx, position.y - 0.5f - ky);
    cubic_source s = {texture, channel};
    ivec2 k = iv2((int)kx, (int)ky);
    if (f.x == 0.0f && f.y == 0.0f) {
        return texel(s, k.x, k.y);
    }
    vec4 wx = catmull_rom(f.x);
    vec4 wy = catmull_rom(f.y);
    vec4 rows = {filtered_row(s, wx, iv2(k.x, k.y - 1)), filtered_row(s, wx, k), filtered_row(s, wx, iv2(k.x, k.y + 1)),
                 filtered_row(s, wx, iv2(k.x, k.y + 2))};
    return nearest_clamp(s, k, dot4(wy, rows));
}
