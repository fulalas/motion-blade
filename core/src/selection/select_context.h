#ifndef MB_SELECT_CONTEXT_H
#define MB_SELECT_CONTEXT_H

#include "common/vec.h"
#include "estimation/window.h"
#include "motionblade/passes.h"

enum { MAX_SELECT_CANDIDATES = 19 };

typedef struct consistency {
    bool a;
    bool b;
} consistency;

typedef struct selection_best {
    float cost;
    vec2 vector;
    float diff_sum;
    bool supported;
    bool consistent;
    int mode;
} selection_best;

typedef struct select_context {
    const mb_select_pass *pass;
    int id;
    ivec2 block;
    vec2 center;
    ivec2 window_origin;
    float t;
    bool has_next;
    bool has_previous;
    vec2 candidates[MAX_SELECT_CANDIDATES];
    float uncover_costs[MAX_SELECT_CANDIDATES];
    int count;
    int two_sided_count;
    float mean_weight;
    bool agreed;
    vec2 agreed_vector;
    bool any_supported;
    float supported_cost;
    vec2 supported_vector;
    selection_best best;
} select_context;

void select_context_begin(select_context *ctx, const mb_select_pass *pass, int id);
void select_add(select_context *ctx, vec2 u);
vec2 select_landing_a(const select_context *ctx, vec2 u);
vec2 select_landing_b(const select_context *ctx, vec2 u);
vec2 select_field_at(const select_context *ctx, const mb_buffer *field, vec2 position);
vec2 select_consistency_errors(const select_context *ctx, vec2 u);
consistency select_consistency(const select_context *ctx, vec2 u);
bool select_supported_previous(const select_context *ctx, vec2 u);
bool select_supported_next(const select_context *ctx, vec2 u);
bool select_supported_both(const select_context *ctx, vec2 u);
sad_result select_pair_sad(const select_context *ctx, vec2 u);
float select_future_sad(const select_context *ctx, vec2 u);

void select_score_two_sided(select_context *ctx);
void select_check_agreed(select_context *ctx);
void select_refine_supported(select_context *ctx);
void select_try_uncover(select_context *ctx);
void select_try_one_sided(select_context *ctx);

#endif
