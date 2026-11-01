#include "common/blocks.h"
#include "reference/cpu_resources.h"
#include "selection/select_context.h"

static const float NOT_FOUND = 1e30f;
static const float SEARCH_LIMIT = 1e29f;
static const float FRAME_MARGIN = 8.0f;

static float foreground_bonus(const select_context *ctx, vec2 u)
{
    return ctx->pass->foreground_bonus * clampf(v2_l1(u) * 0.5f, 0.0f, 1.0f);
}

static int inconsistent_sides(consistency ok)
{
    return (int)!ok.a + (int)!ok.b;
}

typedef struct two_sided_score {
    float cost;
    vec2 vector;
    float diff_sum;
    consistency ok;
} two_sided_score;

static void take_two_sided(select_context *ctx, two_sided_score score)
{
    ctx->best.cost = score.cost;
    ctx->best.vector = score.vector;
    ctx->best.diff_sum = score.diff_sum;
    ctx->best.consistent = score.ok.a && score.ok.b;
}

typedef struct scored {
    float cost;
    bool supported;
} scored;

static scored score_candidate(select_context *ctx, vec2 u, sad_result r, consistency ok)
{
    const mb_select_pass *pass = ctx->pass;
    float penalty = (float)inconsistent_sides(ok);
    bool checked = ctx->any_supported && v2_l1(u) >= pass->support.min_length;
    bool supported = true;
    if (checked) {
        bool previous = select_supported_previous(ctx, u);
        bool next = select_supported_next(ctx, u);
        supported = previous || next;
        if (previous && next && pass->support.waive) {
            penalty = 0.0f;
        }
    }
    float cost = r.sad + pass->penalty_inconsistent * penalty;
    bool far_from_agreed = v2_l1(v2_sub(u, ctx->agreed_vector)) >= pass->support.min_length;
    if (ctx->agreed && far_from_agreed && !select_supported_both(ctx, u)) {
        cost += pass->support.penalty_disagree;
    }
    if (checked && !supported) {
        cost += pass->support.penalty_unsupported;
    } else if (checked && cost < ctx->supported_cost) {
        ctx->supported_cost = cost;
        ctx->supported_vector = u;
    }
    return (scored){cost, supported};
}

void select_score_two_sided(select_context *ctx)
{
    for (int i = 0; i < ctx->two_sided_count; i++) {
        vec2 u = ctx->candidates[i];
        sad_result r = select_pair_sad(ctx, u);
        consistency ok = select_consistency(ctx, u);
        scored s = score_candidate(ctx, u, r, ok);
        if (ok.a && ok.b && s.supported) {
            s.cost -= foreground_bonus(ctx, u);
        }
        ctx->uncover_costs[i] = ok.b && !ok.a ? s.cost : -1.0f;
        if (s.cost < ctx->best.cost) {
            take_two_sided(ctx, (two_sided_score){s.cost, u, r.diff_sum, ok});
            ctx->best.supported = s.supported;
        }
    }
}

void select_check_agreed(select_context *ctx)
{
    vec2 u = ctx->agreed_vector;
    if (!ctx->agreed || v2_l1(v2_sub(ctx->best.vector, u)) < ctx->pass->support.min_length) {
        return;
    }
    sad_result r = select_pair_sad(ctx, u);
    consistency ok = select_consistency(ctx, u);
    float cost = r.sad + ctx->pass->penalty_inconsistent * (float)inconsistent_sides(ok);
    if (cost < ctx->best.cost) {
        take_two_sided(ctx, (two_sided_score){cost, u, r.diff_sum, ok});
        ctx->best.supported = true;
    }
}

static void try_refined(select_context *ctx, vec2 u, bool require_consistent_side)
{
    sad_result r = select_pair_sad(ctx, u);
    consistency ok = select_consistency(ctx, u);
    if (require_consistent_side && !ok.a && !ok.b) {
        return;
    }
    bool waived = select_supported_both(ctx, u) && ctx->pass->support.waive;
    float cost = r.sad + ctx->pass->penalty_inconsistent * (waived ? 0.0f : (float)inconsistent_sides(ok));
    if (ok.a && ok.b) {
        cost -= foreground_bonus(ctx, u);
    }
    if (cost < ctx->best.cost) {
        take_two_sided(ctx, (two_sided_score){cost, u, r.diff_sum, ok});
    }
}

typedef struct refine_grid {
    vec2 base;
    float reach;
    float step;
    bool require_consistent_side;
} refine_grid;

static void refine_around(select_context *ctx, refine_grid grid)
{
    for (float dy = -grid.reach; dy <= grid.reach; dy += grid.step) {
        for (float dx = -grid.reach; dx <= grid.reach; dx += grid.step) {
            try_refined(ctx, v2_add(grid.base, v2(dx, dy)), grid.require_consistent_side);
        }
    }
}

void select_refine_supported(select_context *ctx)
{
    int radius = ctx->pass->support.refine_radius;
    if (!ctx->any_supported || ctx->best.supported || ctx->supported_cost >= SEARCH_LIMIT || radius <= 0) {
        return;
    }
    refine_around(ctx, (refine_grid){ctx->supported_vector, (float)radius, 2.0f, true});
    refine_around(ctx, (refine_grid){ctx->best.vector, 1.0f, 1.0f, false});
}

void select_try_uncover(select_context *ctx)
{
    const mb_uncover_rules *rules = &ctx->pass->uncover;
    if (!rules->enabled || !ctx->has_previous || !ctx->has_next || ctx->best.cost <= rules->min_cost) {
        return;
    }
    float pick_sad = NOT_FOUND;
    float pick_cost = 0.0f;
    vec2 pick = v2(0.0f, 0.0f);
    for (int i = 0; i < ctx->two_sided_count; i++) {
        if (ctx->uncover_costs[i] < 0.0f) {
            continue;
        }
        float sad = select_future_sad(ctx, ctx->candidates[i]);
        if (sad < pick_sad && sad < ctx->best.cost) {
            pick_sad = sad;
            pick = ctx->candidates[i];
            pick_cost = ctx->uncover_costs[i];
        }
    }
    if (pick_sad < NOT_FOUND && pick_sad <= rules->continuity * select_future_sad(ctx, ctx->best.vector)) {
        ctx->best.vector = pick;
        ctx->best.cost = pick_cost;
        ctx->best.mode = MB_MODE_UNCOVER;
        ctx->best.consistent = false;
    }
}

typedef struct one_sided_search {
    float limit;
    float best_ratio;
} one_sided_search;

static bool near_frame_edge(const select_context *ctx, vec2 p)
{
    vec2 end = v2((float)(ctx->pass->grid.blocks_x - 1) * 8.0f, (float)(ctx->pass->grid.blocks_y - 1) * 8.0f);
    return p.x < FRAME_MARGIN || p.y < FRAME_MARGIN || p.x >= end.x || p.y >= end.y;
}

typedef struct one_sided_rows {
    window near;
    window far;
    mb_image *frame;
    vec2 offset;
} one_sided_rows;

static float texture_ratio(const select_context *ctx, const one_sided_rows *rows, float sad)
{
    window below;
    window_load_exact(&below, &rows->frame->texture, ctx->window_origin, v2_add(rows->offset, v2(0.0f, 1.0f)));
    return sad / fmaxf(window_texture(&rows->near, &below), 1.0f);
}

static one_sided_rows load_one_sided(const select_context *ctx, vec2 u, int mode)
{
    const mb_select_pass *pass = ctx->pass;
    bool future = mode == MB_MODE_FUTURE;
    one_sided_rows rows;
    rows.frame = future ? pass->frames.b : pass->frames.a;
    rows.offset = future ? v2_scale(u, 1.0f - ctx->t) : v2_neg(v2_scale(u, ctx->t));
    mb_image *far_frame = future ? pass->frames.c : pass->frames.z;
    vec2 far_offset = future ? v2_scale(u, 2.0f - ctx->t) : v2_neg(v2_scale(u, 1.0f + ctx->t));
    window_load_exact(&rows.near, &rows.frame->texture, ctx->window_origin, rows.offset);
    window_load_exact(&rows.far, &far_frame->texture, ctx->window_origin, far_offset);
    return rows;
}

static void consider_one_sided(select_context *ctx, one_sided_search *search, vec2 u, int mode)
{
    const mb_select_pass *pass = ctx->pass;
    one_sided_rows rows = load_one_sided(ctx, u, mode);
    float sad = window_zsad(&rows.near, &rows.far, ctx->mean_weight).sad;
    if (sad + pass->one_sided.margin >= search->limit) {
        return;
    }
    float ratio = texture_ratio(ctx, &rows, sad);
    if (ratio < pass->one_sided.texture && ratio < search->best_ratio) {
        search->best_ratio = ratio;
        ctx->best.vector = u;
        ctx->best.mode = mode;
    }
}

static void try_one_sided_vector(select_context *ctx, one_sided_search *search, vec2 u)
{
    const mb_select_pass *pass = ctx->pass;
    float t = ctx->t;
    float limit = consistency_limit(pass->consistency, u);
    vec2 pa = select_landing_a(ctx, u);
    vec2 pb = select_landing_b(ctx, u);
    vec2 errors = select_consistency_errors(ctx, u);
    if (errors.x <= limit && errors.y <= limit) {
        return;
    }
    bool future_ok = t >= 0.5f || near_frame_edge(ctx, pa) || t * errors.x >= pass->one_sided.strip;
    bool past_ok = t <= 0.5f || near_frame_edge(ctx, pb) || (1.0f - t) * errors.y >= pass->one_sided.strip;
    if (ctx->has_next && future_ok && v2_l1(v2_sub(select_field_at(ctx, pass->next_forward, pb), u)) <= limit) {
        consider_one_sided(ctx, search, u, MB_MODE_FUTURE);
    }
    if (ctx->has_previous && past_ok && v2_l1(v2_add(select_field_at(ctx, pass->previous_backward, pa), u)) <= limit) {
        consider_one_sided(ctx, search, u, MB_MODE_PAST);
    }
}

void select_try_one_sided(select_context *ctx)
{
    const mb_select_pass *pass = ctx->pass;
    one_sided_search search = {pass->one_sided.ratio * ctx->best.cost, NOT_FOUND};
    bool worth = search.limit > pass->one_sided.margin && ctx->best.cost > pass->one_sided.min_cost;
    if (!(ctx->has_next || ctx->has_previous) || !worth || ctx->best.consistent) {
        return;
    }
    if (ctx->has_next) {
        select_add(ctx, v4_xy(vec4s(pass->next_forward)[ctx->id]));
    }
    if (ctx->has_previous) {
        select_add(ctx, v2_neg(v4_xy(vec4s(pass->previous_backward)[ctx->id])));
    }
    for (int i = 0; i < ctx->count; i++) {
        try_one_sided_vector(ctx, &search, ctx->candidates[i]);
    }
}
