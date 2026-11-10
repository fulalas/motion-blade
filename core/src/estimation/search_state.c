#include "estimation/search_state.h"

#include "common/blocks.h"

void search_state_begin(search_state *state, const ref_texture *origin, ivec2 block)
{
    state->window_origin = window_origin(block);
    window_load_texels(&state->origin, origin, state->window_origin);
    state->mean_weight = 1.0f;
    state->best = (best_match){1e30f, 0.0f, 0.0f, v2(0.0f, 0.0f)};
    state->whole_vector = v2(1e30f, 1e30f);
}

sad_result search_state_sad_hardware(const search_state *state, const ref_texture *target, vec2 v)
{
    window candidate;
    window_load_hardware(&candidate, target, state->window_origin, v);
    return window_zsad(&state->origin, &candidate, state->mean_weight);
}

sad_result search_state_sad_exact(const search_state *state, const ref_texture *target, vec2 v)
{
    window candidate;
    window_load_exact(&candidate, target, state->window_origin, v);
    return window_zsad(&state->origin, &candidate, state->mean_weight);
}

void search_state_consider(search_state *state, vec2 v, sad_result r, float cost)
{
    if (cost < state->best.cost) {
        state->best = (best_match){cost, r.sad, r.diff_sum, v};
    }
}

void search_state_try(search_state *state, const ref_texture *target, vec2 v, float penalty, float length_penalty)
{
    sad_result r = search_state_sad_hardware(state, target, v);
    search_state_consider(state, v, r, r.sad + penalty + length_penalty * v2_l1(v));
}

void search_state_try_sparse(search_state *state, const ref_texture *target, vec2 v, float penalty,
                             float length_penalty, int stride)
{
    window candidate;
    window_load_hardware_sparse(&candidate, target, state->window_origin, v, stride);
    sad_result r = window_zsad_sparse(&state->origin, &candidate, state->mean_weight, stride);
    search_state_consider(state, v, r, r.sad + penalty + length_penalty * v2_l1(v));
}

sad_result search_state_seed_sparse(search_state *state, const ref_texture *target, vec2 v, float length_penalty,
                                    int stride)
{
    window candidate;
    window_load_hardware(&candidate, target, state->window_origin, v);
    sad_result r = window_zsad_sparse(&state->origin, &candidate, state->mean_weight, stride);
    state->best = (best_match){r.sad + length_penalty * v2_l1(v), r.sad, r.diff_sum, v};
    return window_zsad(&state->origin, &candidate, state->mean_weight);
}

static sad_result best_whole(search_state *state, const ref_texture *target)
{
    if (!v2_equal(state->whole_vector, state->best.vector)) {
        state->whole = search_state_sad_hardware(state, target, state->best.vector);
        state->whole_vector = state->best.vector;
    }
    return state->whole;
}

bool search_state_best_below(search_state *state, const ref_texture *target, float limit)
{
    return state->best.sad < limit && best_whole(state, target).sad < limit;
}

void search_state_measure_best(search_state *state, const ref_texture *target)
{
    sad_result r = best_whole(state, target);
    state->best.cost += r.sad - state->best.sad;
    state->best.sad = r.sad;
    state->best.diff_sum = r.diff_sum;
}

void search_state_seed(search_state *state, vec2 v, sad_result r, float length_penalty)
{
    state->best = (best_match){r.sad + length_penalty * v2_l1(v), r.sad, r.diff_sum, v};
}

vec4 search_state_result(const search_state *state)
{
    const best_match *b = &state->best;
    return (vec4){b->vector.x, b->vector.y, b->sad, block_dc(b->diff_sum)};
}

bool search_accepts(const mb_acceptance *rule, float sad, float original)
{
    return sad < rule->max_ratio * original && original - sad > rule->min_gain;
}
