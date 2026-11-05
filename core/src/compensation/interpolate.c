#include "compensation/quad.h"
#include "motionblade/layout.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

static void blend_chroma(const void *context, ivec2 index, ivec2 cp)
{
    const quad_state *q = context;
    quad_store_chroma(&q->pass->warp, cp, quad_blend_chroma(q, index, cp));
}

static void blend_quad(const mb_interpolate_pass *pass, ivec2 q0)
{
    quad_state q;
    quad_begin(&q, pass, q0);
    quad_gather_blocks(&q);
    quad_add_fallback(&q);
    quad_score(&q);
    quad_weigh(&q);
    quad_values blended = quad_blend_luma(&q);
    quad_measure_panels(&q);
    quad_values luma = quad_mix_panels_luma(&q, blended);
    for (int j = 0; j < QUAD_PIXELS; j++) {
        quad_store_luma(&pass->warp, iv2(q0.x + (j & 1), q0.y + (j >> 1)), luma.v[j]);
    }
    quad_visit_chroma(&pass->warp.chroma, q0, blend_chroma, &q);
}

void ref_interpolate(const mb_interpolate_pass *pass)
{
    if (words(pass->warp.stats)[MB_STAT_CUT] != 0) {
        return;
    }
    for (int y = 0; y < pass->warp.height; y += 2) {
        for (int x = 0; x < pass->warp.width; x += 2) {
            blend_quad(pass, iv2(x, y));
        }
    }
}
