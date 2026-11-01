#include <math.h>

#include "common/blocks.h"
#include "estimation/search_state.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { NEIGHBOURHOOD = 5, NEIGHBOURHOOD_SIZE = NEIGHBOURHOOD * NEIGHBOURHOOD, CENTER = NEIGHBOURHOOD_SIZE / 2 };

typedef struct local_block {
    search_state state;
    const mb_local_search_pass *pass;
    ivec2 block;
    vec2 origin;
    vec2 limit;
} local_block;

static float repair_threshold(const mb_local_search_pass *pass)
{
    float blocks = (float)mb_grid_block_count(pass->grid);
    float noise = pass->noise_factor * (float)words(pass->stats)[MB_STAT_ERROR] * 16.0f / blocks;
    return fmaxf(pass->min_cost, noise);
}

static bool inside_frame(const local_block *lb, vec2 u)
{
    vec2 start = v2_add(lb->origin, u);
    return start.x >= 0.0f && start.y >= 0.0f && start.x <= lb->limit.x && start.y <= lb->limit.y;
}

static vec2 neighbour_vector(const local_block *lb, int k)
{
    ivec2 offset = iv2(k % NEIGHBOURHOOD - 2, k / NEIGHBOURHOOD - 2);
    return v4_xy(vec4s(lb->pass->field)[block_index(iv2_add(lb->block, offset), lb->pass->grid)]);
}

static bool seen_before(const local_block *lb, int k, vec2 u)
{
    for (int j = 0; j < k; j++) {
        if (v2_equal(neighbour_vector(lb, j), u)) {
            return true;
        }
    }
    return false;
}

static void try_vector(local_block *lb, vec2 u)
{
    sad_result r = search_state_sad_hardware(&lb->state, &lb->pass->target->texture, u);
    search_state_consider(&lb->state, u, r, r.sad + lb->pass->length_penalty * v2_l1(u));
}

static void try_neighbours(local_block *lb, vec2 current)
{
    for (int k = 0; k < NEIGHBOURHOOD_SIZE; k++) {
        vec2 u = neighbour_vector(lb, k);
        bool known = v2_equal(u, current) || v2_equal(u, lb->state.best.vector);
        if (k == CENTER || known || !inside_frame(lb, u) || seen_before(lb, k, u)) {
            continue;
        }
        try_vector(lb, u);
    }
}

static void try_grid(local_block *lb)
{
    float radius = lb->pass->radius;
    float step = lb->pass->step;
    vec2 seed = lb->state.best.vector;
    for (float dy = -radius; dy <= radius; dy += step) {
        for (float dx = -radius; dx <= radius; dx += step) {
            vec2 u = v2_add(seed, v2(dx, dy));
            if (!v2_equal(u, lb->state.best.vector) && inside_frame(lb, u)) {
                try_vector(lb, u);
            }
        }
    }
}

static void try_ring(local_block *lb)
{
    vec2 seed = lb->state.best.vector;
    for (int k = 0; k < 8; k++) {
        try_vector(lb, v2_add(seed, v2((float)RING8[k].x, (float)RING8[k].y)));
    }
}

static bool supported_in_time(const local_block *lb, vec2 u)
{
    const mb_local_search_pass *pass = lb->pass;
    int index = block_index(lb->block, pass->grid);
    if (pass->reference_at_landing) {
        index = block_index(block_at(v2_add(block_center(lb->block), u)), pass->grid);
    }
    vec2 reference = v4_xy(vec4s(pass->reference)[index]);
    return v2_l1(v2_add(reference, u)) <= pass->support.rel * v2_l1(u) + pass->support.abs;
}

static vec4 repair_block(const mb_local_search_pass *pass, int id)
{
    local_block lb = {.pass = pass, .block = block_coord(id, pass->grid)};
    search_state_begin(&lb.state, &pass->origin->texture, lb.block);
    ivec2 o = lb.state.window_origin;
    lb.origin = v2((float)o.x, (float)o.y);
    lb.limit = v2((float)(pass->grid.width - MB_WINDOW_SIZE), (float)(pass->grid.height - MB_WINDOW_SIZE));
    vec4 current = vec4s(pass->field)[id];
    vec2 v = v4_xy(current);
    sad_result original = search_state_sad_hardware(&lb.state, &pass->target->texture, v);
    lb.state.best = (best_match){original.sad + pass->length_penalty * v2_l1(v), original.sad, original.diff_sum, v};
    try_neighbours(&lb, v);
    try_grid(&lb);
    try_ring(&lb);
    bool ok = search_accepts(&pass->acceptance, lb.state.best.sad, original.sad);
    if (ok && pass->reference) {
        ok = supported_in_time(&lb, lb.state.best.vector);
    }
    return ok ? search_state_result(&lb.state) : current;
}

void ref_local_search(const mb_local_search_pass *pass)
{
    const vec4 *field = vec4s(pass->field);
    vec4 *repaired = vec4s(pass->repaired);
    float threshold = repair_threshold(pass);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        repaired[id] = field[id].z > threshold ? repair_block(pass, id) : field[id];
    }
}
