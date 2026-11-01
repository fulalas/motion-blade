#include "selection/select_context.h"

#include "common/blocks.h"
#include "reference/cpu_resources.h"

static bool stats_say_cut(const mb_buffer *stats)
{
    return words(stats)[MB_STAT_CUT] != 0;
}

void select_context_begin(select_context *ctx, const mb_select_pass *pass, int id)
{
    ctx->pass = pass;
    ctx->id = id;
    ctx->block = block_coord(id, pass->grid);
    ctx->center = block_center(ctx->block);
    ctx->window_origin = window_origin(ctx->block);
    ctx->t = pass->t;
    ctx->has_next = pass->has_next && !stats_say_cut(pass->next_stats);
    ctx->has_previous = pass->has_previous && !stats_say_cut(pass->previous_stats);
    ctx->count = 0;
    ctx->two_sided_count = 0;
    ctx->mean_weight = 1.0f;
    ctx->agreed = false;
    ctx->any_supported = false;
    ctx->supported_cost = 1e30f;
    ctx->supported_vector = v2(0.0f, 0.0f);
    ctx->best = (selection_best){1e30f, v2(0.0f, 0.0f), 0.0f, true, false, MB_MODE_TWO_SIDED};
}

void select_add(select_context *ctx, vec2 u)
{
    if (!v2_listed(ctx->candidates, ctx->count, u)) {
        ctx->candidates[ctx->count++] = u;
    }
}

vec2 select_landing_a(const select_context *ctx, vec2 u)
{
    return v2_sub(ctx->center, v2_scale(u, ctx->t));
}

vec2 select_landing_b(const select_context *ctx, vec2 u)
{
    return v2_add(ctx->center, v2_scale(u, 1.0f - ctx->t));
}

vec2 select_field_at(const select_context *ctx, const mb_buffer *field, vec2 position)
{
    int index = block_index(block_at(position), ctx->pass->grid);
    return v4_xy(vec4s(field)[index]);
}

vec2 select_consistency_errors(const select_context *ctx, vec2 u)
{
    float a = v2_l1(v2_sub(select_field_at(ctx, ctx->pass->forward, select_landing_a(ctx, u)), u));
    float b = v2_l1(v2_add(select_field_at(ctx, ctx->pass->backward, select_landing_b(ctx, u)), u));
    return v2(a, b);
}

consistency select_consistency(const select_context *ctx, vec2 u)
{
    vec2 errors = select_consistency_errors(ctx, u);
    float limit = consistency_limit(ctx->pass->consistency, u);
    return (consistency){errors.x <= limit, errors.y <= limit};
}

static float support_tolerance(const select_context *ctx, vec2 u)
{
    return ctx->pass->support.tolerance.rel * v2_l1(u) + ctx->pass->support.tolerance.abs;
}

bool select_supported_previous(const select_context *ctx, vec2 u)
{
    vec2 previous = select_field_at(ctx, ctx->pass->previous_backward, select_landing_a(ctx, u));
    return v2_l1(v2_add(previous, u)) <= support_tolerance(ctx, u);
}

bool select_supported_next(const select_context *ctx, vec2 u)
{
    vec2 next = select_field_at(ctx, ctx->pass->next_forward, select_landing_b(ctx, u));
    return v2_l1(v2_sub(next, u)) <= support_tolerance(ctx, u);
}

bool select_supported_both(const select_context *ctx, vec2 u)
{
    return select_supported_previous(ctx, u) && select_supported_next(ctx, u);
}

sad_result select_pair_sad(const select_context *ctx, vec2 u)
{
    window a;
    window b;
    window_load_exact(&a, &ctx->pass->frames.a->texture, ctx->window_origin, v2_neg(v2_scale(u, ctx->t)));
    window_load_exact(&b, &ctx->pass->frames.b->texture, ctx->window_origin, v2_scale(u, 1.0f - ctx->t));
    return window_zsad(&a, &b, ctx->mean_weight);
}

float select_future_sad(const select_context *ctx, vec2 u)
{
    window b;
    window c;
    window_load_exact(&b, &ctx->pass->frames.b->texture, ctx->window_origin, v2_scale(u, 1.0f - ctx->t));
    window_load_exact(&c, &ctx->pass->frames.c->texture, ctx->window_origin, v2_scale(u, 2.0f - ctx->t));
    return window_zsad(&b, &c, ctx->mean_weight).sad;
}
