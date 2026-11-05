#include "compensation/chroma.h"
#include "compensation/extrapolate.h"
#include "compensation/quad.h"
#include "motionblade/layout.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

static vec2 chroma_texel(const mb_frame_planes *planes, const mb_chroma_layout *chroma, ivec2 p)
{
    chroma_channels c = chroma_channels_of(planes, chroma);
    return v2(ref_texel(c.first, p.x, p.y, 0), ref_texel(c.second, p.x, p.y, c.second_channel));
}

typedef struct quad_copy {
    const mb_interpolate_pass *pass;
    const mb_frame_planes *source;
} quad_copy;

static void copy_chroma(const void *context, ivec2 index, ivec2 cp)
{
    (void)index;
    const quad_copy *copy = context;
    quad_store_chroma_raw(copy->pass, cp, chroma_texel(copy->source, &copy->pass->chroma, cp));
}

static void copy_quad(const quad_copy *copy, ivec2 q0)
{
    const mb_interpolate_pass *pass = copy->pass;
    for (int j = 0; j < QUAD_PIXELS; j++) {
        ivec2 p = iv2(q0.x + (j & 1), q0.y + (j >> 1));
        if (p.x < pass->width && p.y < pass->height) {
            ref_store(&pass->output.luma->texture, p.x, p.y, 0, ref_texel(&copy->source->luma->texture, p.x, p.y, 0));
        }
    }
    quad_visit_chroma(&pass->chroma, q0, copy_chroma, copy);
}

static void copy_frame(const mb_interpolate_pass *pass, const mb_frame_planes *source)
{
    quad_copy copy = {pass, source};
    for (int y = 0; y < pass->height; y += 2) {
        for (int x = 0; x < pass->width; x += 2) {
            copy_quad(&copy, iv2(x, y));
        }
    }
}

static void blend_chroma(const void *context, ivec2 index, ivec2 cp)
{
    const quad_state *q = context;
    quad_store_chroma(q->pass, cp, quad_blend_chroma(q, index, cp));
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
        quad_store_luma(pass, iv2(q0.x + (j & 1), q0.y + (j >> 1)), luma.v[j]);
    }
    quad_visit_chroma(&pass->chroma, q0, blend_chroma, &q);
}

static void blend(const mb_interpolate_pass *pass)
{
    for (int y = 0; y < pass->height; y += 2) {
        for (int x = 0; x < pass->width; x += 2) {
            blend_quad(pass, iv2(x, y));
        }
    }
}

static void bridge_cut(const mb_interpolate_pass *pass)
{
    cut_plan plan = cut_plan_of(pass);
    if (plan.moves) {
        ref_extrapolate(pass, plan.from_b);
    } else {
        copy_frame(pass, plan.from_b ? &pass->frames.b : &pass->frames.a);
    }
}

void ref_interpolate(const mb_interpolate_pass *pass)
{
    if (words(pass->stats)[MB_STAT_CUT] == 0) {
        blend(pass);
    } else {
        bridge_cut(pass);
    }
}
