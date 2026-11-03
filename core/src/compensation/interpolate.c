#include "common/blocks.h"
#include "compensation/chroma.h"
#include "compensation/cubic.h"
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

static const mb_frame_planes *nearest_frame(const mb_interpolate_pass *pass)
{
    return pass->t < MB_NEAREST_SWITCH ? &pass->frames.a : &pass->frames.b;
}

static void copy_nearest(const mb_interpolate_pass *pass)
{
    const mb_frame_planes *source = nearest_frame(pass);
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

static bool can_extrapolate(const mb_interpolate_pass *pass)
{
    return pass->extrapolation.field && words(pass->extrapolation.stats)[MB_STAT_CUT] == 0;
}

static vec2 field_vector(const mb_interpolate_pass *pass, vec2 p)
{
    float scale = (float)(1 << pass->base_level);
    block_taps taps = block_taps_at(v2(p.x / scale, p.y / scale), pass->grid);
    vec2 sum = v2(0.0f, 0.0f);
    for (int k = 0; k < 4; k++) {
        sum = v2_add(sum, v2_scale(v4_xy(vec4s(pass->extrapolation.field)[taps.index[k]]), taps.weight[k]));
    }
    return v2_scale(sum, scale);
}

static vec2 extrapolated_offset(const mb_interpolate_pass *pass, vec2 p)
{
    float step = pass->t < MB_NEAREST_SWITCH ? pass->t : 1.0f - pass->t;
    vec2 first = v2_scale(field_vector(pass, p), step);
    return v2_scale(field_vector(pass, v2_add(p, first)), step);
}

typedef struct quad_offsets {
    vec2 v[QUAD_PIXELS];
} quad_offsets;

static quad_offsets extrapolate_luma(const mb_interpolate_pass *pass, ivec2 q0)
{
    const ref_texture *source = &nearest_frame(pass)->luma->texture;
    quad_offsets offsets;
    for (int j = 0; j < QUAD_PIXELS; j++) {
        ivec2 p = iv2(q0.x + (j & 1), q0.y + (j >> 1));
        vec2 pc = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
        offsets.v[j] = extrapolated_offset(pass, pc);
        store_luma(pass, p, cubic_sample(source, v2_add(pc, offsets.v[j]), 0));
    }
    return offsets;
}

static vec2 covered_offset(const mb_chroma_layout *chroma, const quad_offsets *offsets, ivec2 index)
{
    covered_pixels cover = quad_covered_pixels(chroma, index);
    vec2 sum = v2(0.0f, 0.0f);
    for (int k = 0; k < cover.count; k++) {
        sum = v2_add(sum, offsets->v[cover.index[k]]);
    }
    return v2_scale(sum, 1.0f / (float)cover.count);
}

static void extrapolate_chroma(const mb_interpolate_pass *pass, ivec2 q0, const quad_offsets *offsets)
{
    const mb_chroma_layout *chroma = &pass->chroma;
    for (int cy = 0; cy < (2 >> chroma->shift_y); cy++) {
        for (int cx = 0; cx < (2 >> chroma->shift_x); cx++) {
            ivec2 cp = iv2((q0.x >> chroma->shift_x) + cx, (q0.y >> chroma->shift_y) + cy);
            if (cp.x < chroma->width && cp.y < chroma->height) {
                vec2 pc = v2((float)cp.x + 0.5f, (float)cp.y + 0.5f);
                vec2 at = v2_add(pc, chroma_from_luma(chroma, covered_offset(chroma, offsets, iv2(cx, cy))));
                store_chroma(pass, cp, chroma_cubic_sample(nearest_frame(pass), chroma, at));
            }
        }
    }
}

static void extrapolate_quad(const mb_interpolate_pass *pass, ivec2 q0)
{
    quad_offsets offsets = extrapolate_luma(pass, q0);
    extrapolate_chroma(pass, q0, &offsets);
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

static void for_each_quad(const mb_interpolate_pass *pass, void (*draw)(const mb_interpolate_pass *, ivec2))
{
    for (int y = 0; y < pass->height; y += 2) {
        for (int x = 0; x < pass->width; x += 2) {
            draw(pass, iv2(x, y));
        }
    }
}

void ref_interpolate(const mb_interpolate_pass *pass)
{
    if (words(pass->stats)[MB_STAT_CUT] == 0) {
        for_each_quad(pass, blend_quad);
    } else if (can_extrapolate(pass)) {
        for_each_quad(pass, extrapolate_quad);
    } else {
        copy_nearest(pass);
    }
}
