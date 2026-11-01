#include "compensation/chroma.h"
#include "compensation/quad.h"
#include "motionblade/layout.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

static void store_luma(const mb_interpolate_pass *pass, ivec2 p, float value)
{
    if (p.x < pass->width && p.y < pass->height) {
        ref_store(&pass->output.luma->texture, p.x, p.y, 0, quad_output_value(&pass->range, value));
    }
}

static void store_chroma_raw(const mb_interpolate_pass *pass, ivec2 p, vec2 value)
{
    chroma_channels out = chroma_channels_of(&pass->output, &pass->chroma);
    ref_store(out.first, p.x, p.y, 0, value.x);
    ref_store(out.second, p.x, p.y, out.second_channel, value.y);
}

static void store_chroma(const mb_interpolate_pass *pass, ivec2 p, vec2 value)
{
    store_chroma_raw(pass, p, v2(quad_output_value(&pass->range, value.x), quad_output_value(&pass->range, value.y)));
}

static vec2 chroma_texel(const mb_frame_planes *planes, const mb_chroma_layout *chroma, ivec2 p)
{
    chroma_channels c = chroma_channels_of(planes, chroma);
    return v2(ref_texel(c.first, p.x, p.y, 0), ref_texel(c.second, p.x, p.y, c.second_channel));
}

static void copy_nearest(const mb_interpolate_pass *pass)
{
    const mb_frame_planes *source = pass->t < 0.5f ? &pass->frames.a : &pass->frames.b;
    for (int y = 0; y < pass->height; y++) {
        for (int x = 0; x < pass->width; x++) {
            ref_store(&pass->output.luma->texture, x, y, 0, ref_texel(&source->luma->texture, x, y, 0));
        }
    }
    for (int y = 0; y < pass->chroma.height; y++) {
        for (int x = 0; x < pass->chroma.width; x++) {
            store_chroma_raw(pass, iv2(x, y), chroma_texel(source, &pass->chroma, iv2(x, y)));
        }
    }
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
        store_luma(pass, iv2(q0.x + (j & 1), q0.y + (j >> 1)), luma.v[j]);
    }
    for (int cy = 0; cy < (2 >> pass->chroma.shift_y); cy++) {
        for (int cx = 0; cx < (2 >> pass->chroma.shift_x); cx++) {
            ivec2 cp = iv2((q0.x >> pass->chroma.shift_x) + cx, (q0.y >> pass->chroma.shift_y) + cy);
            if (cp.x < pass->chroma.width && cp.y < pass->chroma.height) {
                store_chroma(pass, cp, quad_blend_chroma(&q, iv2(cx, cy), cp));
            }
        }
    }
}

void ref_interpolate(const mb_interpolate_pass *pass)
{
    if (words(pass->stats)[MB_STAT_CUT] != 0) {
        copy_nearest(pass);
        return;
    }
    for (int y = 0; y < pass->height; y += 2) {
        for (int x = 0; x < pass->width; x += 2) {
            blend_quad(pass, iv2(x, y));
        }
    }
}
