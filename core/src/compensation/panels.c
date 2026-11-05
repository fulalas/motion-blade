#include <stdlib.h>

#include "common/blocks.h"
#include "compensation/chroma.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { SAMPLES_PER_SIDE = 16, SAMPLES = SAMPLES_PER_SIDE * SAMPLES_PER_SIDE, PROBES = 9 };

static int compare_floats(const void *a, const void *b)
{
    float x = *(const float *)a;
    float y = *(const float *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static float middle_of_sorted(float values[SAMPLES])
{
    qsort(values, SAMPLES, sizeof(float), compare_floats);
    return 0.5f * (values[SAMPLES / 2 - 1] + values[SAMPLES / 2]);
}

void ref_global_motion(const mb_global_motion_pass *pass)
{
    const vec4 *selection = vec4s(pass->selection);
    int bw = pass->grid.blocks_x;
    int bh = pass->grid.blocks_y;
    float xs[SAMPLES];
    float ys[SAMPLES];
    for (int t = 0; t < SAMPLES; t++) {
        int bx = ((t & 15) * 2 + 1) * bw / 32;
        int by = ((t >> 4) * 2 + 1) * bh / 32;
        xs[t] = selection[by * bw + bx].x;
        ys[t] = selection[by * bw + bx].y;
    }
    vec4s(pass->global_motion)[0] = (vec4){middle_of_sorted(xs), middle_of_sorted(ys), 0.0f, 0.0f};
}

typedef struct panel_probe {
    float chroma_change;
    float saturation_a;
    float saturation_b;
    vec2 luma_lo;
    vec2 luma_hi;
} panel_probe;

static float max_component(vec2 v)
{
    return fmaxf(v.x, v.y);
}

static vec2 abs2(vec2 v)
{
    return v2(fabsf(v.x), fabsf(v.y));
}

static void probe(const mb_panel_pass *pass, panel_probe *acc, vec2 p, vec2 u)
{
    vec2 pa = v2_sub(p, v2_scale(u, pass->t));
    vec2 pb = v2_add(p, v2_scale(u, 1.0f - pass->t));
    vec2 ca = chroma_sample(&pass->frames.a, &pass->chroma, chroma_from_luma(&pass->chroma, pa));
    vec2 cb = chroma_sample(&pass->frames.b, &pass->chroma, chroma_from_luma(&pass->chroma, pb));
    vec2 mid = v2(pass->chroma_mid, pass->chroma_mid);
    acc->chroma_change += max_component(abs2(v2_sub(ca, cb)));
    acc->saturation_a += max_component(abs2(v2_sub(ca, mid)));
    acc->saturation_b += max_component(abs2(v2_sub(cb, mid)));
    vec2 inv = v2_inverse_size(pass->width, pass->height);
    float a = ref_sample(&pass->frames.a.luma->texture, pa.x * inv.x, pa.y * inv.y, 0);
    float b = ref_sample(&pass->frames.b.luma->texture, pb.x * inv.x, pb.y * inv.y, 0);
    acc->luma_lo = v2(fminf(acc->luma_lo.x, a), fminf(acc->luma_lo.y, b));
    acc->luma_hi = v2(fmaxf(acc->luma_hi.x, a), fmaxf(acc->luma_hi.y, b));
}

static vec2 panel_flag(const mb_panel_pass *pass, int id)
{
    float m = (float)(1 << pass->base_level);
    vec2 u = v2_scale(v4_xy(vec4s(pass->global_motion)[0]), m);
    ivec2 block = block_coord(id, pass->grid);
    vec2 corner = block_corner(block, m);
    panel_probe acc = {0.0f, 0.0f, 0.0f, v2(1e30f, 1e30f), v2(-1e30f, -1e30f)};
    for (int k = 0; k < PROBES; k++) {
        vec2 grid = v2_add(v2_scale(v2((float)(k % 3), (float)(k / 3)), 3.0f), v2(1.0f, 1.0f));
        probe(pass, &acc, v2_add(corner, v2_scale(grid, m)), u);
    }
    float scale = pass->level_scale;
    float change = acc.chroma_change / 9.0f * scale;
    float luma_range = fmaxf(acc.luma_hi.x - acc.luma_lo.x, acc.luma_hi.y - acc.luma_lo.y) * scale;
    float flat = 1.0f - smoothstepf(pass->flat * 0.5f, pass->flat, luma_range);
    float saturated = smoothstepf(pass->saturation * 0.5f, pass->saturation,
                                  fminf(acc.saturation_a, acc.saturation_b) / 9.0f * scale);
    return v2(smoothstepf(pass->change_lo, pass->change_hi, change) * flat * saturated, flat);
}

void ref_panels(const mb_panel_pass *pass)
{
    vec2 *flags = vec2s(pass->flags);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        flags[id] = pass->change_hi > 0.0f ? panel_flag(pass, id) : v2(0.0f, 0.0f);
    }
}

static float flagged_sources(const mb_panel_spread_pass *pass, ivec2 block)
{
    const vec2 *flags = vec2s(pass->flags);
    const vec4 *selection = vec4s(pass->selection);
    vec2 global = v4_xy(vec4s(pass->global_motion)[0]);
    vec2 c = block_center(block);
    float strongest = 0.0f;
    for (int e = 0; e < PROBES && pass->source_distance > 0.0f; e++) {
        vec2 u = v4_xy(selection[neighbour3_index(e, block, pass->grid)]);
        if (v2_l1(v2_sub(u, global)) <= pass->source_distance) {
            continue;
        }
        for (int k = 0; k < 2; k++) {
            float scale = k == 0 ? -pass->t : 1.0f - pass->t;
            ivec2 q = block_at(v2_add(c, v2_scale(u, scale)));
            for (int d = 0; d < PROBES; d++) {
                strongest = fmaxf(strongest, flags[neighbour3_index(d, q, pass->grid)].x);
            }
        }
    }
    return strongest;
}

static float nearby_flag(const mb_panel_spread_pass *pass, ivec2 block)
{
    const vec2 *flags = vec2s(pass->flags);
    float strongest = 0.0f;
    for (int y = -pass->radius; y <= pass->radius; y++) {
        for (int x = -pass->radius; x <= pass->radius; x++) {
            strongest = fmaxf(strongest, flags[block_index(iv2_add(block, iv2(x, y)), pass->grid)].x);
        }
    }
    return strongest;
}

void ref_panel_spread(const mb_panel_spread_pass *pass)
{
    const vec2 *flags = vec2s(pass->flags);
    float *spread = floats(pass->spread);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        ivec2 block = block_coord(id, pass->grid);
        float own = fmaxf(flags[id].x, nearby_flag(pass, block) * flags[id].y);
        spread[id] = fmaxf(own, flagged_sources(pass, block));
    }
}
