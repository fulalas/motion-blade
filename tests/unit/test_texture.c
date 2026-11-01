#include <math.h>

#include "check.h"
#include "common/texture.h"

static bool half_keeps_exact_values(void)
{
    CHECK(ref_half(1.0f) == 1.0f);
    CHECK(ref_half(0.5f) == 0.5f);
    CHECK(ref_half(0.0f) == 0.0f);
    CHECK(ref_half(65504.0f) == 65504.0f);
    return true;
}

static bool half_rounds_to_nearest_even(void)
{
    float step = ldexpf(1.0f, -10);
    CHECK(ref_half(1.0f + step * 0.5f) == 1.0f);
    CHECK(ref_half(1.0f + step * 1.5f) == 1.0f + 2.0f * step);
    CHECK(fabsf(ref_half(0.1f) - 0.1f) < 1e-4f);
    return true;
}

static bool unorm_quantizes(void)
{
    CHECK(ref_quantize(MB_TEXELS_R8, 1.2f) == 1.0f);
    CHECK(ref_quantize(MB_TEXELS_R8, -0.1f) == 0.0f);
    CHECK(ref_quantize(MB_TEXELS_R8, 0.5f) == 128.0f / 255.0f);
    return true;
}

static bool samples_texel_centers_exactly(void)
{
    ref_texture t;
    mb_image_desc desc = {4, 4, MB_TEXELS_R8};
    CHECK(ref_texture_alloc(&t, &desc));
    for (int i = 0; i < 16; i++) {
        t.texels[i] = (float)i / 255.0f;
    }
    CHECK(ref_sample(&t, 2.5f / 4.0f, 1.5f / 4.0f, 0) == ref_texel(&t, 2, 1, 0));
    CHECK(ref_sample(&t, 2.0f / 4.0f, 1.5f / 4.0f, 0) == (ref_texel(&t, 1, 1, 0) + ref_texel(&t, 2, 1, 0)) * 0.5f);
    ref_texture_free(&t);
    return true;
}

static bool clamps_to_edge(void)
{
    ref_texture t;
    mb_image_desc desc = {2, 2, MB_TEXELS_R8};
    CHECK(ref_texture_alloc(&t, &desc));
    t.texels[0] = 0.25f;
    CHECK(ref_texel(&t, -5, -5, 0) == 0.25f);
    ref_texture_free(&t);
    return true;
}

int main(void)
{
    static const test_case tests[] = {
        {"half keeps exact values", half_keeps_exact_values},
        {"half rounds to nearest even", half_rounds_to_nearest_even},
        {"unorm quantizes", unorm_quantizes},
        {"samples texel centers exactly", samples_texel_centers_exactly},
        {"clamps to edge", clamps_to_edge},
    };
    return RUN_ALL(tests);
}
