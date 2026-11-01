#ifndef MB_VEC_H
#define MB_VEC_H

#include <math.h>
#include <stdbool.h>

typedef struct vec2 {
    float x;
    float y;
} vec2;

typedef struct vec4 {
    float x;
    float y;
    float z;
    float w;
} vec4;

typedef struct ivec2 {
    int x;
    int y;
} ivec2;

static inline vec2 v2(float x, float y)
{
    return (vec2){x, y};
}

static inline vec2 v2_add(vec2 a, vec2 b)
{
    return (vec2){a.x + b.x, a.y + b.y};
}

static inline vec2 v2_sub(vec2 a, vec2 b)
{
    return (vec2){a.x - b.x, a.y - b.y};
}

static inline vec2 v2_scale(vec2 a, float s)
{
    return (vec2){a.x * s, a.y * s};
}

static inline vec2 v2_neg(vec2 a)
{
    return (vec2){-a.x, -a.y};
}

static inline vec2 v2_inverse_size(int width, int height)
{
    return v2(1.0f / (float)width, 1.0f / (float)height);
}

static inline bool v2_equal(vec2 a, vec2 b)
{
    return a.x == b.x && a.y == b.y;
}

static inline bool v2_listed(const vec2 *list, int count, vec2 a)
{
    for (int k = 0; k < count; k++) {
        if (v2_equal(list[k], a)) {
            return true;
        }
    }
    return false;
}

static inline float v2_l1(vec2 a)
{
    return fabsf(a.x) + fabsf(a.y);
}

static inline vec2 v4_xy(vec4 a)
{
    return (vec2){a.x, a.y};
}

static inline ivec2 iv2(int x, int y)
{
    return (ivec2){x, y};
}

static inline ivec2 iv2_add(ivec2 a, ivec2 b)
{
    return (ivec2){a.x + b.x, a.y + b.y};
}

static inline ivec2 iv2_floor(vec2 a)
{
    return (ivec2){(int)floorf(a.x), (int)floorf(a.y)};
}

static inline float clampf(float value, float lo, float hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

static inline int clampi(int value, int lo, int hi)
{
    return value < lo ? lo : value > hi ? hi : value;
}

static inline float smoothstepf(float edge0, float edge1, float x)
{
    float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float mixf(float a, float b, float weight)
{
    return a * (1.0f - weight) + b * weight;
}

#endif
