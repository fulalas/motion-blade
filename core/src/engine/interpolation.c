#include "engine/interpolation.h"

#include "engine/geometry.h"
#include "engine/motion.h"

typedef struct pair_fields {
    mb_buffer *forward;
    mb_buffer *backward;
    mb_buffer *next_forward;
    mb_buffer *previous_backward;
} pair_fields;

typedef struct pass_context {
    mb_engine *engine;
    const pair_choice *pair;
    pair_fields fields;
    double exact_t;
    float t;
    mb_buffer *selection;
} pass_context;

static const pair_motion *ready_pair(const mb_engine *engine, int back)
{
    const pair_motion *m = back >= 0 && back < PAIR_RING ? pair_at(engine, back) : NULL;
    return m && m->ready ? m : NULL;
}

pair_choice choose_pair(const mb_engine *engine, int end)
{
    const pair_motion *next = ready_pair(engine, end - 1);
    const pair_motion *past = ready_pair(engine, end + 1);
    return (pair_choice){
        .a = ring_index(engine, end + 1),
        .b = ring_index(engine, end),
        .c = next ? ring_index(engine, end - 1) : -1,
        .z = past ? ring_index(engine, end + 2) : -1,
        .current = pair_at(engine, end),
        .next = next,
        .past = past,
    };
}

static mb_buffer *base_field(const mb_engine *engine, const pair_motion *m, int direction)
{
    if (!m) {
        return NULL;
    }
    int base = engine->geo.base_level;
    return direction ? m->backward[base] : m->forward[base];
}

static mb_buffer *stats_of(const pair_motion *m)
{
    return m ? m->stats : NULL;
}

static mb_image *side_level(const mb_engine *engine, int slot, int level)
{
    return slot < 0 ? NULL : level_image(engine, slot, level);
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
    const pair_fields *f = &ctx->fields;
    b->ops->fill_buffer(b->self, s->keys, MB_KEY_NONE);
    b->ops->fill_buffer(b->self, s->extra_keys, MB_KEY_NONE);
    if (ctx->engine->params.select.temporal_candidates) {
        if (f->previous_backward) {
            project(ctx, (mb_project_pass){.field = f->previous_backward, .keys = s->extra_keys,
                                           .time_scale = (float)-ctx->exact_t, .tag = 0});
        }
        if (f->next_forward) {
            project(ctx, (mb_project_pass){.field = f->next_forward, .keys = s->extra_keys,
                                           .time_scale = (float)-(1.0 - ctx->exact_t), .tag = 1});
        }
    }
    project(ctx, (mb_project_pass){.field = f->forward, .keys = s->keys, .time_scale = ctx->t, .tag = 0});
    project(ctx, (mb_project_pass){.field = f->backward, .keys = s->keys, .time_scale = (float)(1.0 - ctx->exact_t),
                                   .tag = 1});
}

static mb_support_rules support_rules(const mb_select_params *s)
{
    return (mb_support_rules){
        s->temporal_support != 0, support_tolerance_of(s), s->penalty_unsupported, s->support_min_length,
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
        .frames = {level_image(e, p->a, base), level_image(e, p->b, base), side_level(e, p->c, base),
                   side_level(e, p->z, base)},
        .forward = ctx->fields.forward,
        .backward = ctx->fields.backward,
        .keys = e->sel.keys,
        .selection = e->sel.selection[0],
        .next_forward = ctx->fields.next_forward,
        .previous_backward = ctx->fields.previous_backward,
        .next_stats = stats_of(p->next),
        .previous_stats = stats_of(p->past),
        .extra_keys = e->sel.extra_keys,
        .grid = base_grid(ctx),
        .t = ctx->t,
        .has_next = p->next != NULL,
        .has_previous = p->past != NULL,
        .penalty_inconsistent = s->penalty_inconsistent,
        .consistency = s->consistency_threshold,
        .foreground_bonus = s->foreground_bonus,
        .mean_removal = mean_removal_of(&e->params.search),
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
        ctx->selection, ctx->engine->sel.fallback, ctx->fields.forward, ctx->fields.backward, base_grid(ctx), ctx->t,
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

static mb_extrapolation extrapolation_of(const pass_context *ctx)
{
    const pair_choice *p = ctx->pair;
    return (mb_extrapolation){
        {ctx->fields.previous_backward, stats_of(p->past), side_level(ctx->engine, p->z, 0)},
        {ctx->fields.next_forward, stats_of(p->next), side_level(ctx->engine, p->c, 0)},
    };
}

static mb_warp warp_of(const pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    const pair_choice *p = ctx->pair;
    return (mb_warp){
        .frames = pair_planes(ctx),
        .a_level = level_image(e, p->a, 0),
        .b_level = level_image(e, p->b, 0),
        .output = e->output,
        .stats = p->current->stats,
        .width = e->geo.format.width,
        .height = e->geo.format.height,
        .grid = base_grid(ctx),
        .chroma = e->geo.chroma,
        .base_level = e->geo.base_level,
        .t = ctx->t,
        .range = geometry_output_range(&e->geo),
    };
}

static void blend(const pass_context *ctx)
{
    mb_engine *e = ctx->engine;
    mb_interpolate_pass pass = {
        .warp = warp_of(ctx),
        .cells = e->sel.cells,
        .forward = ctx->fields.forward,
        .backward = ctx->fields.backward,
        .fallback = e->sel.fallback,
        .offsets = e->sel.offsets_median,
        .panels = e->sel.panel_spread,
        .global_motion = e->sel.global_motion,
        .luma_scale = geometry_luma_scale(&e->geo),
        .costs = blend_costs(e),
    };
    backend_of(ctx)->ops->interpolate(backend_of(ctx)->self, &pass);
}

static void bridge_cut(const pass_context *ctx)
{
    const mb_engine *e = ctx->engine;
    mb_extrapolate_pass pass = {
        .warp = warp_of(ctx),
        .extrapolation = extrapolation_of(ctx),
        .softness = e->params.blend.softness,
        .unconfirmed = e->params.blend.penalty_unconfirmed,
        .consistency = e->params.select.consistency_threshold,
    };
    backend_of(ctx)->ops->extrapolate(backend_of(ctx)->self, &pass);
}

void record_interpolation(mb_engine *engine, const pair_choice *pair, double t)
{
    pair_fields fields = {
        base_field(engine, pair->current, 0),
        base_field(engine, pair->current, 1),
        base_field(engine, pair->next, 0),
        base_field(engine, pair->past, 1),
    };
    pass_context ctx = {engine, pair, fields, t, (float)t, NULL};
    engine->debug.forward = fields.forward;
    engine->debug.backward = fields.backward;
    project_candidates(&ctx);
    select_vectors(&ctx);
    smooth_vectors(&ctx);
    engine->debug.selection = ctx.selection;
    weigh_fallback(&ctx);
    measure_light(&ctx);
    find_panels(&ctx);
    split_blocks(&ctx);
    blend(&ctx);
    bridge_cut(&ctx);
}
