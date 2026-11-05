#include <stdlib.h>

#include "common/blocks.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { SPAN = 7, REACH = 3 };

static float inconsistency(const mb_fallback_pass *pass, ivec2 block)
{
    int index = block_index(block, pass->grid);
    vec4 selected = vec4s(pass->selection)[index];
    if (!is_two_sided(selected)) {
        return 0.0f;
    }
    vec2 u = v4_xy(selected);
    ivec2 b = block_coord(index, pass->grid);
    vec2 c = block_center(b);
    vec2 forward = field_at_position(vec4s(pass->forward), v2_sub(c, v2_scale(u, pass->t)), pass->grid);
    vec2 backward = field_at_position(vec4s(pass->backward), v2_add(c, v2_scale(u, 1.0f - pass->t)), pass->grid);
    float ia = v2_l1(v2_sub(forward, u));
    float ib = v2_l1(v2_add(backward, u));
    float relative = fminf(ia, ib) / (1.0f + 0.1f * v2_l1(u));
    return smoothstepf(pass->lo, pass->hi, relative);
}

static float spread_weight(const mb_fallback_pass *pass, int distance)
{
    return distance == 0 ? 1.0f : distance == 1 ? pass->spread : pass->spread * pass->spread;
}

static float fallback_weight(const mb_fallback_pass *pass, ivec2 block)
{
    float raw[SPAN][SPAN];
    for (int y = 0; y < SPAN; y++) {
        for (int x = 0; x < SPAN; x++) {
            raw[y][x] = inconsistency(pass, iv2(block.x + x - REACH, block.y + y - REACH));
        }
    }
    float weight = 0.0f;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            float sum = 0.0f;
            for (int k = 0; k < 9; k++) {
                ivec2 o = neighbour3(k);
                sum += raw[dy + REACH + o.y][dx + REACH + o.x];
            }
            int distance = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            weight = fmaxf(weight, sum / 9.0f * spread_weight(pass, distance));
        }
    }
    return weight;
}

void ref_fallback(const mb_fallback_pass *pass)
{
    const vec4 *selection = vec4s(pass->selection);
    float *weights = floats(pass->weights);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        bool uncovered = (int)selection[id].z == MB_MODE_UNCOVER;
        weights[id] = uncovered ? 0.0f : fallback_weight(pass, block_coord(id, pass->grid));
    }
}
