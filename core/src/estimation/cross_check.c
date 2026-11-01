#include "common/blocks.h"
#include "estimation/search_state.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

typedef struct reverse_candidates {
    vec2 vectors[MB_KEYS_PER_BLOCK];
    int count;
} reverse_candidates;

static bool window_fits(vec2 origin, vec2 u, mb_grid grid)
{
    vec2 start = v2_add(origin, u);
    vec2 end = v2_add(start, v2((float)MB_WINDOW_SIZE, (float)MB_WINDOW_SIZE));
    return start.x >= 0.0f && start.y >= 0.0f && end.x <= (float)grid.width && end.y <= (float)grid.height;
}

static reverse_candidates gather(const mb_cross_check_pass *pass, int id, vec2 current)
{
    reverse_candidates list = {.count = 0};
    ivec2 o = window_origin(block_coord(id, pass->grid));
    vec2 origin = v2((float)o.x, (float)o.y);
    const uint32_t *keys = &words(pass->keys)[id * MB_KEYS_PER_BLOCK];
    for (int k = 0; k < MB_KEYS_PER_BLOCK && keys[k] != MB_KEY_NONE; k++) {
        vec2 u = v2_neg(v4_xy(vec4s(pass->other)[keys[k] >> 1]));
        if (v2_l1(v2_sub(u, current)) >= pass->min_change && window_fits(origin, u, pass->grid)) {
            list.vectors[list.count++] = u;
        }
    }
    return list;
}

static vec4 repair_block(const mb_cross_check_pass *pass, int id, vec4 current, const reverse_candidates *list)
{
    search_state state;
    search_state_begin(&state, &pass->origin->texture, block_coord(id, pass->grid));
    vec2 v = v4_xy(current);
    float original = search_state_sad_exact(&state, &pass->target->texture, v).sad;
    state.best = (best_match){original + pass->length_penalty * v2_l1(v), original, 0.0f, v};
    bool moved = false;
    for (int k = 0; k < list->count; k++) {
        vec2 u = list->vectors[k];
        sad_result r = search_state_sad_exact(&state, &pass->target->texture, u);
        float cost = r.sad + pass->penalty + pass->length_penalty * v2_l1(u);
        if (cost < state.best.cost && search_accepts(&pass->acceptance, r.sad, original)) {
            state.best = (best_match){cost, r.sad, r.diff_sum, u};
            moved = true;
        }
    }
    return moved ? search_state_result(&state) : current;
}

void ref_cross_check(const mb_cross_check_pass *pass)
{
    const vec4 *field = vec4s(pass->field);
    vec4 *repaired = vec4s(pass->repaired);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        reverse_candidates list = gather(pass, id, v4_xy(field[id]));
        repaired[id] = list.count ? repair_block(pass, id, field[id], &list) : field[id];
    }
}
