#include "blocks.glsl"

layout(binding = 0) uniform sampler2D a_luma;
layout(binding = 1) uniform sampler2D b_luma;
layout(binding = 2) uniform sampler2D a_chroma1;
layout(binding = 3) uniform sampler2D b_chroma1;
layout(binding = 4) uniform sampler2D a_chroma2;
layout(binding = 5) uniform sampler2D b_chroma2;
layout(binding = 12) uniform sampler2D a_level;
layout(binding = 13) uniform sampler2D b_level;
#ifdef DEPTH16
layout(binding = 9, r16) uniform writeonly image2D out_luma;
#ifdef NV
layout(binding = 10, rg16) uniform writeonly image2D out_chroma1;
#else
layout(binding = 10, r16) uniform writeonly image2D out_chroma1;
layout(binding = 11, r16) uniform writeonly image2D out_chroma2;
#endif
#else
layout(binding = 9, r8) uniform writeonly image2D out_luma;
#ifdef NV
layout(binding = 10, rg8) uniform writeonly image2D out_chroma1;
#else
layout(binding = 10, r8) uniform writeonly image2D out_chroma1;
layout(binding = 11, r8) uniform writeonly image2D out_chroma2;
#endif
#endif

#define FIELD_FORWARD 0
#define FIELD_BACKWARD 1
#define FIELD_PAST 2
#define FIELD_NEXT 3

float level_scale;
vec2 texel_size;
float differences[16];

vec4 catmull_rom(float f)
{
    return vec4(f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f), f * (0.5 + f * (2.0 - 1.5 * f)),
                f * f * (-0.5 + 0.5 * f));
}

float cubic_patch(vec4 a, vec4 b, vec4 c, vec4 d, vec4 wx, vec4 wy)
{
    float v = dot(wy, vec4(dot(wx, vec4(a.w, a.z, b.w, b.z)), dot(wx, vec4(a.x, a.y, b.x, b.y)),
                           dot(wx, vec4(c.w, c.z, d.w, d.z)), dot(wx, vec4(c.x, c.y, d.x, d.y))));
    return clamp(v, min(min(a.y, b.x), min(c.z, d.w)), max(max(a.y, b.x), max(c.z, d.w)));
}

vec2 cubic(sampler2D T, vec2 pos, vec2 texel, bool two)
{
    vec2 k = floor(pos - 0.5), f = pos - 0.5 - k;
    if (f == vec2(0.0)) {
        return texelFetch(T, clamp(ivec2(k), ivec2(0), textureSize(T, 0) - 1), 0).rg;
    }
    vec4 wx = catmull_rom(f.x), wy = catmull_rom(f.y);
    vec2 t0 = k * texel, t2 = (k + 2.0) * texel;
    vec2 r = vec2(cubic_patch(textureGather(T, t0, 0), textureGather(T, vec2(t2.x, t0.y), 0),
                              textureGather(T, vec2(t0.x, t2.y), 0), textureGather(T, t2, 0), wx, wy),
                  0.0);
    if (two) {
        r.y = cubic_patch(textureGather(T, t0, 1), textureGather(T, vec2(t2.x, t0.y), 1),
                          textureGather(T, vec2(t0.x, t2.y), 1), textureGather(T, t2, 1), wx, wy);
    }
    return r;
}

void gather_row_pair(sampler2D T, vec2 corner, vec2 texel, vec4 wx, int gy, inout float rows[5][2],
                     inout float inner[3][3])
{
    vec4 g0 = textureGather(T, corner * texel, 0);
    vec4 g1 = textureGather(T, (corner + vec2(2.0, 0.0)) * texel, 0);
    vec4 g2 = textureGather(T, (corner + vec2(4.0, 0.0)) * texel, 0);
    float r[2][6] = float[2][6](float[6](g0.w, g0.z, g1.w, g1.z, g2.w, g2.z),
                                float[6](g0.x, g0.y, g1.x, g1.y, g2.x, g2.y));
    for (int rr = 0; rr < 2; rr++) {
        int row = 2 * gy + rr;
        if (row > 4) {
            continue;
        }
        for (int jx = 0; jx < 2; jx++) {
            rows[row][jx] = dot(wx, vec4(r[rr][jx], r[rr][jx + 1], r[rr][jx + 2], r[rr][jx + 3]));
        }
        if (row >= 1 && row <= 3) {
            for (int c = 0; c < 3; c++) {
                inner[row - 1][c] = r[rr][c + 1];
            }
        }
    }
}

void quad_cubic(sampler2D T, vec2 pos, vec2 texel, out float o[4])
{
    vec2 k = floor(pos - 0.5), f = pos - 0.5 - k;
    if (f == vec2(0.0)) {
        ivec2 last = textureSize(T, 0) - 1;
        for (int j = 0; j < 4; j++) {
            o[j] = texelFetch(T, clamp(ivec2(k) + ivec2(j & 1, j >> 1), ivec2(0), last), 0).r;
        }
        return;
    }
    vec4 wx = catmull_rom(f.x), wy = catmull_rom(f.y);
    float rows[5][2], inner[3][3];
    for (int gy = 0; gy < 3; gy++) {
        gather_row_pair(T, k + vec2(0.0, float(2 * gy)), texel, wx, gy, rows, inner);
    }
    for (int j = 0; j < 4; j++) {
        int jx = j & 1, jy = j >> 1;
        float v = dot(wy, vec4(rows[jy][jx], rows[jy + 1][jx], rows[jy + 2][jx], rows[jy + 3][jx]));
        float lo = min(min(inner[jy][jx], inner[jy][jx + 1]), min(inner[jy + 1][jx], inner[jy + 1][jx + 1]));
        float hi = max(max(inner[jy][jx], inner[jy][jx + 1]), max(inner[jy + 1][jx], inner[jy + 1][jx + 1]));
        o[j] = clamp(v, lo, hi);
    }
}

float output_value(float v)
{
#ifdef DEPTH16
    v = min(v, push.max_value);
    if (push.quant_step > 0.0) {
        v = round(v / push.quant_step) * push.quant_step;
    }
#endif
    return v;
}

vec2 probed_vector(int field, int i);

float consistent_share(int field, vec2 p, vec2 u)
{
    BlockTaps taps = taps_at(p / level_scale, blocks());
    u /= level_scale;
    float limit = consistency_limit(push.consistency, u);
    float share = 0.0;
    for (int k = 0; k < 4; k++) {
        share += l1(probed_vector(field, taps.index[k]) - u) <= limit ? taps.weight[k] : 0.0;
    }
    return share;
}

float soft_score(float cost, float lowest)
{
    float e = exp(-(cost - lowest) / push.softness);
    return e < MB_WEIGHT_CUTOFF ? 0.0 : e;
}

vec2 window_error(int jx, int jy)
{
    float sum = 0.0, lowest = 1e30;
    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
            float d = differences[(jy + dy) * 4 + jx + dx];
            sum += d;
            lowest = min(lowest, d);
        }
    }
    return vec2(sum, lowest);
}

vec2 chroma_shift()
{
    return vec2(1 << push.shift_x, 1 << push.shift_y);
}

vec2 chroma_texel_size()
{
    return 1.0 / vec2(push.chroma_width, push.chroma_height);
}

struct CoveredPixels {
    int index[4];
    int count;
};

CoveredPixels covered_pixels(ivec2 index)
{
    CoveredPixels cover;
    cover.count = 0;
    for (int yy = 0; yy < (1 << push.shift_y); yy++) {
        for (int xx = 0; xx < (1 << push.shift_x); xx++) {
            cover.index[cover.count++] = ((index.y << push.shift_y) + yy) * 2 + (index.x << push.shift_x) + xx;
        }
    }
    return cover;
}

vec2 cubic_chroma(sampler2D first, sampler2D second, vec2 at)
{
#ifdef NV
    return cubic(first, at, chroma_texel_size(), true);
#else
    vec2 texel = chroma_texel_size();
    return vec2(cubic(first, at, texel, false).x, cubic(second, at, texel, false).x);
#endif
}

void store_chroma(ivec2 cp, vec2 value)
{
#ifdef NV
    imageStore(out_chroma1, cp, vec4(output_value(value.x), output_value(value.y), 0.0, 0.0));
#else
    imageStore(out_chroma1, cp, vec4(output_value(value.x)));
    imageStore(out_chroma2, cp, vec4(output_value(value.y)));
#endif
}
