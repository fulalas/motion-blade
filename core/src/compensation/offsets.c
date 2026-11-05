#include "common/blocks.h"
#include "compensation/chroma.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { NEIGHBOURS = 9 };

static vec2 chroma_offset(const mb_chroma_offset_pass *pass, int id, vec2 u)
{
    const mb_chroma_layout *chroma = &pass->chroma;
    float m = (float)(1 << pass->base_level);
    ivec2 block = block_coord(id, pass->grid);
    vec2 corner = block_corner(block, m);
    vec2 scaled = v2_scale(u, m);
    vec2 c0 = chroma_from_luma(chroma, corner);
    vec2 to_a = chroma_from_luma(chroma, v2_scale(scaled, -pass->t));
    vec2 to_b = chroma_from_luma(chroma, v2_scale(scaled, 1.0f - pass->t));
    int nx = 4 >> chroma->shift_x;
    int ny = 4 >> chroma->shift_y;
    vec2 sum = v2(0.0f, 0.0f);
    for (int y = 0; y < ny; y++) {
        for (int x = 0; x < nx; x++) {
            vec2 p = v2_add(c0, v2_scale(v2((float)(2 * x + 1), (float)(2 * y + 1)), m));
            vec2 a = chroma_sample(&pass->frames.a, chroma, v2_add(p, to_a));
            vec2 b = chroma_sample(&pass->frames.b, chroma, v2_add(p, to_b));
            sum = v2_add(sum, v2_sub(a, b));
        }
    }
    return v2_scale(sum, 1.0f / (float)(nx * ny));
}

void ref_chroma_offsets(const mb_chroma_offset_pass *pass)
{
    const vec4 *selection = vec4s(pass->selection);
    vec4 *offsets = vec4s(pass->offsets);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        vec2 dc = chroma_offset(pass, id, v4_xy(selection[id]));
        offsets[id] = (vec4){selection[id].w / (255.0f * pass->luma_scale), dc.x, dc.y, 0.0f};
    }
}

static float channel(vec4 v, int c)
{
    return c == 0 ? v.x : c == 1 ? v.y : v.z;
}

static float channel_median(const vec4 values[NEIGHBOURS], int c)
{
    float median = 0.0f;
    for (int i = 0; i < NEIGHBOURS; i++) {
        int below = 0;
        float vi = channel(values[i], c);
        for (int j = 0; j < NEIGHBOURS; j++) {
            float vj = channel(values[j], c);
            below += vj < vi || (vj == vi && j < i);
        }
        if (below == NEIGHBOURS / 2) {
            median = vi;
        }
    }
    return median;
}

void ref_offset_median(const mb_offset_median_pass *pass)
{
    const vec4 *offsets = vec4s(pass->offsets);
    vec4 *filtered = vec4s(pass->filtered);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        vec4 values[NEIGHBOURS];
        for (int k = 0; k < NEIGHBOURS; k++) {
            values[k] = offsets[neighbour3_index(k, block_coord(id, pass->grid), pass->grid)];
        }
        filtered[id] = (vec4){channel_median(values, 0), channel_median(values, 1), channel_median(values, 2), 0.0f};
    }
}
