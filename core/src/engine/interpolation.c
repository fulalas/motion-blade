#include "engine/interpolation.h"

#include "engine/geometry.h"
#include "engine/motion.h"

typedef struct pass_context {
    mb_engine *engine;
    const pair_choice *pair;
    double exact_t;
    float t;
    mb_buffer *selection;
} pass_context;

pair_choice choose_pair(const mb_engine *engine, bool lookahead)
{
    const motion_fields *f = &engine->fields;
    int base = engine->geo.base_level;
    pair_choice p;
    if (lookahead) {
        p = (pair_choice){
            .a = ring_index(engine, 2),
            .b = ring_index(engine, 1),
            .c = ring_index(engine, 0),
            .z = ring_index(engine, 3),
            .forward = f->previous_forward[base],
            .backward = f->previous_backward[base],
            .next_forward = f->forward[base],
            .previous_backward = f->older_backward,
            .current_stats = f->previous_stats,
            .previous_stats = f->older_stats,
            .lookahead = true,
            .past = engine->pair.has_older,
        };
    } else {
        p = (pair_choice){
            .a = ring_index(engine, 1),
            .b = ring_index(engine, 0),
            .c = ring_index(engine, 0),
            .z = ring_index(engine, 2),
            .forward = f->forward[base],
            .backward = f->backward[base],
            .next_forward = NULL,
            .previous_backward = f->previous_backward[base],
            .current_stats = f->stats,
            .previous_stats = f->previous_stats,
            .lookahead = false,
            .past = engine->pair.has_previous,
        };
    }
    if (!p.past) {
        p.z = p.a;
        p.previous_backward = NULL;
        p.previous_stats = p.current_stats;
    }
    return p;
}

static const mb_backend *backend_of(const pass_context *ctx)
{
    return &ctx->engine->backend;
}

static mb_grid base_grid(const pass_context *ctx)
{
    return geometry_base_grid(&ctx->engine->geo);
}

static void project(const pass_context *ctx, mb_project_pass pass)
{
    pass.grid = base_grid(ctx);
    backend_of(ctx)->ops->project(backend_of(ctx)->self, &pass);
}

static void project_candidates(const pass_context *ctx)
{
    const mb_backend *b = backend_of(ctx);
    const selection_buffers *s = &ctx->engine->sel;
    const pair_choice *p = ctx->pair;
    b->ops->fill_buffer(b->self, s->keys, MB_KEY_NONE);
    b->ops->fill_buffer(b->self, s->extra_keys, MB_KEY_NONE);
    if (ctx->engine->params.select.temporal_candidates) {
        if (p->past) {
            project(ctx, (mb_project_pass){.field = p->previous_backward, .keys = s->extra_keys,
                                           .time_scale = (float)-ctx->exact_t, .tag = 0});
        }
        if (p->lookahead) {
            project(ctx, (mb_project_pass){.field = p->next_forward, .keys = s->extra_keys,
                                           .time_scale = (float)-(1.0 - ctx->exact_t), .tag = 1});
        }
    }
    project(ctx, (mb_project_pass){.field = p->forward, .keys = s->keys, .time_scale = ctx->t, .tag = 0});
    project(ctx, (mb_project_pass){.field = p->backward, .keys = s->keys, .time_scale = (float)(1.0 - ctx->exact_t),
                                   .tag = 1});
}

static mb_support_rules support_rules(const mb_select_params *s)
{
    return (mb_support_rules){
        s->temporal_support != 0, {s->support_rel, s->support_abs}, s->penalty_unsupported, s->support_min_length,
        s->support_refine_radius, s->support_waive != 0, s->penalty_disagree,
    };
}

static void select_vectors(pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    const mb_select_params *s = &e->params.select;
    const pair_choice *p = ctx->pair;
    int base = e->geo.base_level;
    mb_select_pass pass = {
        .frames = {level_image(e, p->a, base), level_image(e, p->b, base), level_image(e, p->c, base),
                   level_image(e, p->z, base)},
        .forward = p->forward,
        .backward = p->backward,
        .keys = e->sel.keys,
        .selection = e->sel.selection[0],
        .next_forward = p->next_forward,
        .previous_backward = p->previous_backward,
        .next_stats = e->fields.stats,
        .previous_stats = p->previous_stats,
        .extra_keys = e->sel.extra_keys,
        .grid = base_grid(ctx),
        .t = ctx->t,
        .has_next = p->lookahead,
        .has_previous = p->past,
        .penalty_inconsistent = s->penalty_inconsistent,
        .consistency = s->consistency_threshold,
        .foreground_bonus = s->foreground_bonus,
        .mean_removal = {e->params.search.mean_removal_lo, e->params.search.mean_removal_hi},
        .support = support_rules(s),
        .one_sided = {s->one_sided_margin, s->one_sided_ratio, s->one_sided_min_cost, s->one_sided_texture,
                      s->occlusion_strip},
        .uncover = {s->uncover != 0, s->uncover_min_cost, s->uncover_continuity},
    };
    backend_of(ctx)->ops->select(backend_of(ctx)->self, &pass);
    ctx->selection = e->sel.selection[0];
}

static void smooth_vectors(pass_context *ctx)
{
    float threshold = ctx->engine->params.select.median_threshold;
    if (threshold <= 0.0f) {
        return;
    }
    mb_median_pass pass = {ctx->selection, ctx->engine->sel.selection[1], base_grid(ctx), threshold};
    backend_of(ctx)->ops->median(backend_of(ctx)->self, &pass);
    ctx->selection = ctx->engine->sel.selection[1];
}

static void weigh_fallback(const pass_context *ctx)
{
    const mb_select_params *s = &ctx->engine->params.select;
    mb_fallback_pass pass = {
        ctx->selection, ctx->engine->sel.fallback, ctx->pair->forward, ctx->pair->backward, base_grid(ctx), ctx->t,
        s->fallback_spread, s->fallback_lo, s->fallback_hi,
    };
    backend_of(ctx)->ops->fallback(backend_of(ctx)->self, &pass);
}

static mb_pair_planes pair_planes(const pass_context *ctx)
{
    return (mb_pair_planes){ctx->engine->frames[ctx->pair->a].planes, ctx->engine->frames[ctx->pair->b].planes};
}

static void measure_light(const pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    const mb_backend *b = backend_of(ctx);
    mb_chroma_offset_pass offsets = {
        pair_planes(ctx), ctx->selection, e->sel.offsets, base_grid(ctx), e->geo.chroma, e->geo.base_level, ctx->t,
        geometry_luma_scale(&e->geo),
    };
    b->ops->chroma_offsets(b->self, &offsets);
    mb_offset_median_pass median = {e->sel.offsets, e->sel.offsets_median, base_grid(ctx)};
    b->ops->offset_median(b->self, &median);
}

static void find_panels(const pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    const mb_backend *b = backend_of(ctx);
    const mb_panel_params *p = &e->params.panel;
    mb_global_motion_pass global = {ctx->selection, e->sel.global_motion, base_grid(ctx)};
    b->ops->global_motion(b->self, &global);
    mb_panel_pass panels = {
        pair_planes(ctx), e->sel.panel_flags, e->sel.global_motion, base_grid(ctx), e->geo.format.width,
        e->geo.format.height, e->geo.chroma, e->geo.base_level, ctx->t, 255.0f * geometry_luma_scale(&e->geo),
        p->change_lo, p->change_hi, p->flat, geometry_chroma_mid(&e->geo), p->saturation,
    };
    b->ops->panels(b->self, &panels);
    mb_panel_spread_pass spread = {
        e->sel.panel_flags, e->sel.panel_spread, ctx->selection, e->sel.global_motion, base_grid(ctx), p->radius,
        p->source_distance, ctx->t,
    };
    b->ops->panel_spread(b->self, &spread);
}

static void split_blocks(const pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    const mb_select_params *s = &e->params.select;
    int base = e->geo.base_level;
    mb_split_pass pass = {
        level_image(e, ctx->pair->a, base), level_image(e, ctx->pair->b, base), ctx->selection, e->sel.cells,
        e->sel.offsets_median, base_grid(ctx), s->split_refine_radius, ctx->t,
        s->split_threshold / (float)(1 << base), geometry_luma_scale(&e->geo), s->split_refine_penalty,
    };
    backend_of(ctx)->ops->split(backend_of(ctx)->self, &pass);
}

static mb_blend_costs blend_costs(const mb_engine *e)
{
    const mb_blend_params *b = &e->params.blend;
    return (mb_blend_costs){
        .softness = b->softness,
        .one_sided = b->penalty_one_sided,
        .unconfirmed = b->penalty_unconfirmed,
        .fallback = b->penalty_fallback,
        .fallback_uncover = b->penalty_fallback_uncover,
        .candidate_prior = b->candidate_prior,
        .consistency = e->params.select.consistency_threshold,
        .foreground_bonus = b->foreground_bonus,
        .nearest_side_lo = b->nearest_side_lo,
        .nearest_side_hi = b->nearest_side_hi,
        .panel_switch = e->params.panel.switch_level,
    };
}

static void blend(const pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    const pair_choice *p = ctx->pair;
    mb_interpolate_pass pass = {
        .frames = pair_planes(ctx),
        .a_level = level_image(e, p->a, 0),
        .b_level = level_image(e, p->b, 0),
        .output = e->output,
        .cells = e->sel.cells,
        .forward = p->forward,
        .backward = p->backward,
        .stats = p->current_stats,
        .fallback = e->sel.fallback,
        .offsets = e->sel.offsets_median,
        .panels = e->sel.panel_spread,
        .global_motion = e->sel.global_motion,
        .width = e->geo.format.width,
        .height = e->geo.format.height,
        .grid = base_grid(ctx),
        .chroma = e->geo.chroma,
        .base_level = e->geo.base_level,
        .t = ctx->t,
        .luma_scale = geometry_luma_scale(&e->geo),
        .range = geometry_output_range(&e->geo),
        .costs = blend_costs(e),
    };
    backend_of(ctx)->ops->interpolate(backend_of(ctx)->self, &pass);
}

void record_interpolation(mb_engine *engine, const pair_choice *pair, double t)
{
    pass_context ctx = {engine, pair, t, (float)t, NULL};
    engine->debug.forward = pair->forward;
    engine->debug.backward = pair->backward;
    project_candidates(&ctx);
    select_vectors(&ctx);
    smooth_vectors(&ctx);
    engine->debug.selection = ctx.selection;
    weigh_fallback(&ctx);
    measure_light(&ctx);
    find_panels(&ctx);
    split_blocks(&ctx);
    blend(&ctx);
}
