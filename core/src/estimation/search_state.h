#ifndef MB_SEARCH_STATE_H
#define MB_SEARCH_STATE_H

#include "estimation/window.h"
#include "motionblade/passes.h"

typedef struct best_match {
    float cost;
    float sad;
    float diff_sum;
    vec2 vector;
} best_match;

typedef struct search_state {
    window origin;
    ivec2 window_origin;
    float mean_weight;
    best_match best;
} search_state;

void search_state_begin(search_state *state, const ref_texture *origin, ivec2 block);
sad_result search_state_sad_hardware(const search_state *state, const ref_texture *target, vec2 v);
sad_result search_state_sad_exact(const search_state *state, const ref_texture *target, vec2 v);
void search_state_consider(search_state *state, vec2 v, sad_result r, float cost);
void search_state_try(search_state *state, const ref_texture *target, vec2 v, float penalty,
                      float length_penalty);
void search_state_seed(search_state *state, vec2 v, sad_result r, float length_penalty);
vec4 search_state_result(const search_state *state);
bool search_accepts(const mb_acceptance *rule, float sad, float original);

#endif
