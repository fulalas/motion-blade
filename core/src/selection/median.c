#include "common/blocks.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { NEIGHBOURS = 9 };

typedef struct neighbour_set {
    vec4 vectors[NEIGHBOURS];
    int count;
} neighbour_set;

static neighbour_set two_sided_neighbours(const vec4 *selection, ivec2 block, mb_grid grid)
{
    neighbour_set set = {.count = 0};
    for (int k = 0; k < NEIGHBOURS; k++) {
        ivec2 q = iv2_add(block, neighbour3(k));
        if (block_inside(q, grid) && is_two_sided(selection[block_index(q, grid)])) {
            set.vectors[set.count++] = selection[block_index(q, grid)];
        }
    }
    return set;
}

static vec4 vector_median(const neighbour_set *set, vec4 fallback)
{
    vec4 median = fallback;
    float best = 1e30f;
    for (int i = 0; i < set->count; i++) {
        float distance = 0.0f;
        for (int j = 0; j < set->count; j++) {
            distance += v2_l1(v2_sub(v4_xy(set->vectors[i]), v4_xy(set->vectors[j])));
        }
        if (distance < best) {
            best = distance;
            median = set->vectors[i];
        }
    }
    return median;
}

void ref_median(const mb_median_pass *pass)
{
    const vec4 *selection = vec4s(pass->selection);
    vec4 *filtered = vec4s(pass->filtered);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        vec4 v = selection[id];
        if (!is_two_sided(v)) {
            filtered[id] = v;
            continue;
        }
        neighbour_set set = two_sided_neighbours(selection, block_coord(id, pass->grid), pass->grid);
        vec4 median = vector_median(&set, v);
        filtered[id] = v2_l1(v2_sub(v4_xy(median), v4_xy(v))) > pass->threshold ? median : v;
    }
}
