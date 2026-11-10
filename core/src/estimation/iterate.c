#include <math.h>

#include "common/blocks.h"
#include "estimation/search_state.h"
#include "estimation/window.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { MAX_CANDIDATES = 20, NEIGHBOURS = 9 };

static const float EARLY_EXIT_FLOOR = 32.0f;

typedef struct candidate_list {
    vec2 vectors[MAX_CANDIDATES];
    float penalties[MAX_CANDIDATES];
    int count;
} candidate_list;

static void add_candidate(candidate_list *list, vec2 v, float penalty)
{
    if (v2_listed(list->vectors, list->count, v)) {
        return;
    }
    list->vectors[list->count] = v;
    list->penalties[list->count] = penalty;
    list->count++;
}

static mb_grid parent_grid(const mb_iterate_pass *pass)
{
    return (mb_grid){pass->parent_blocks_x, pass->parent_blocks_y, 0, 0};
}

static vec2 parent_vector(const mb_iterate_pass *pass, ivec2 parent_block)
{
    return v2_scale(v4_xy(vec4s(pass->parent)[block_index(parent_block, parent_grid(pass))]), 2.0f);
}

static vec2 previous_vector(const mb_iterate_pass *pass, ivec2 block)
{
    ivec2 b = iv2(clampi(block.x, 0, pass->grid.blocks_x - 1), clampi(block.y, 0, pass->grid.blocks_y - 1));
    if (pass->previous) {
        return v4_xy(vec4s(pass->previous)[block_index(b, pass->grid)]);
    }
    return parent_vector(pass, iv2(b.x >> 1, b.y >> 1));
}

static float neighbour_dc(const mb_iterate_pass *pass, ivec2 block)
{
    float smallest = 1e30f;
    for (int k = 0; k < NEIGHBOURS; k++) {
        float dc = 0.0f;
        if (pass->previous) {
            dc = vec4s(pass->previous)[neighbour3_index(k, block, pass->grid)].w;
        } else if (pass->parent) {
            dc = vec4s(pass->parent)[neighbour3_index(k, iv2(block.x >> 1, block.y >> 1), parent_grid(pass))].w;
        }
        smallest = fminf(smallest, fabsf(dc));
    }
    return smallest;
}

static void add_parent_neighbours(candidate_list *list, const mb_iterate_pass *pass, ivec2 block)
{
    ivec2 p = iv2(block.x >> 1, block.y >> 1);
    ivec2 q = iv2((block.x & 1) ? p.x + 1 : p.x - 1, (block.y & 1) ? p.y + 1 : p.y - 1);
    add_candidate(list, parent_vector(pass, iv2(q.x, p.y)), pass->penalties.parent);
    add_candidate(list, parent_vector(pass, iv2(p.x, q.y)), pass->penalties.parent);
    add_candidate(list, parent_vector(pass, q), pass->penalties.parent);
}

static bool previous_pair_was_cut(const mb_iterate_pass *pass)
{
    return floats(pass->history)[MB_HIST_LAST_CUT] != 0.0f;
}

static candidate_list gather_candidates(const mb_iterate_pass *pass, int id, ivec2 block)
{
    candidate_list list = {.count = 0};
    add_candidate(&list, previous_vector(pass, block), 0.0f);
    for (int k = 0; k < 8; k++) {
        add_candidate(&list, previous_vector(pass, iv2_add(block, RING8[k])), 0.0f);
    }
    if (pass->parent && !pass->previous) {
        add_parent_neighbours(&list, pass, block);
    }
    if (pass->temporal && !previous_pair_was_cut(pass)) {
        add_candidate(&list, v4_xy(vec4s(pass->temporal)[id]), pass->penalties.temporal);
    }
    if (pass->reverse) {
        add_candidate(&list, v2_neg(v4_xy(vec4s(pass->reverse)[id])), pass->penalties.reverse);
    }
    add_candidate(&list, v2(0.0f, 0.0f), pass->penalties.zero);
    return list;
}

static float early_exit_limit(const window *origin, float factor)
{
    if (factor <= 0.0f) {
        return -1.0f;
    }
    float mean = window_sum(origin) / (float)(MB_WINDOW_SIZE * MB_WINDOW_SIZE);
    float lanes[MB_WINDOW_SIZE];
    for (int lane = 0; lane < MB_WINDOW_SIZE; lane++) {
        float sum = 0.0f;
        for (int x = 0; x < MB_WINDOW_SIZE; x++) {
            sum += fabsf(origin->rows[lane][x] - mean);
        }
        lanes[lane] = sum;
    }
    return fmaxf(cluster_sum(lanes) * 255.0f * factor, EARLY_EXIT_FLOOR);
}

static uint32_t mix_bits(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void random_steps(uint32_t id, uint32_t seed, vec2 steps[2])
{
    uint32_t h = mix_bits(id * 7919u + seed * 104729u);
    steps[0] = v2_scale(v2((float)(int)(h & 7u) - 3.5f, (float)(int)((h >> 3) & 7u) - 3.5f), 0.25f);
    steps[1] = v2((float)(int)((h >> 6) & 3u) - 1.5f, (float)(int)((h >> 8) & 3u) - 1.5f);
}

static void try_candidate(search_state *state, const mb_iterate_pass *pass, vec2 v, float penalty)
{
    search_state_try_sparse(state, &pass->target->texture, v, penalty, pass->penalties.length, MB_ITERATE_STRIDE);
}

static vec4 search_block(const mb_iterate_pass *pass, int id)
{
    ivec2 block = block_coord(id, pass->grid);
    search_state state;
    search_state_begin(&state, &pass->origin->texture, block);
    state.mean_weight = mean_weight(pass->mean_removal, neighbour_dc(pass, block));
    candidate_list list = gather_candidates(pass, id, block);
    float limit = early_exit_limit(&state.origin, pass->early_exit);
    for (int i = 0; i < list.count; i++) {
        try_candidate(&state, pass, list.vectors[i], list.penalties[i]);
        if (state.best.sad < limit) {
            break;
        }
    }
    vec2 steps[2];
    random_steps((uint32_t)id, pass->seed, steps);
    vec2 base = state.best.vector;
    for (int k = 0; k < 2 && state.best.sad >= limit; k++) {
        try_candidate(&state, pass, v2_add(base, steps[k]), pass->penalties.random);
    }
    search_state_measure_best(&state, &pass->target->texture);
    return search_state_result(&state);
}

void ref_iterate_search(const mb_iterate_pass *pass)
{
    vec4 *field = vec4s(pass->field);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        field[id] = search_block(pass, id);
    }
}
