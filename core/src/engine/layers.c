#include "engine/layers.h"

#include "engine/geometry.h"
#include "engine/motion.h"

static int top_level(const geometry *g)
{
    return g->level_count - 1;
}

static int mask_level(const geometry *g)
{
    return g->level_count > 1 ? g->level_count - 2 : 0;
}

static int detail_level(const geometry *g)
{
    int below = mask_level(g) - 1;
    return below > g->base_level ? below : g->base_level;
}

static mb_layer_grid layer_grid(const geometry *g)
{
    mb_grid top = g->grids[top_level(g)];
    return (mb_layer_grid){(top.width + MB_LAYER_CELL - 1) / MB_LAYER_CELL, (top.height + MB_LAYER_CELL - 1) / MB_LAYER_CELL,
                           top.width, top.height};
}

size_t layer_cells(const geometry *g)
{
    mb_layer_grid grid = layer_grid(g);
    return (size_t)grid.cells_x * (size_t)grid.cells_y;
}

size_t layer_mask_texels(const geometry *g)
{
    mb_grid mask = g->grids[mask_level(g)];
    return (size_t)mask.width * (size_t)mask.height;
}

static bool layers_enabled(const mb_engine *engine)
{
    const geometry *g = &engine->geo;
    return engine->params.layer.enabled && g->level_count - g->base_level >= 2;
}

static void fit_model(mb_engine *engine, int direction, mb_buffer *model)
{
    const pair_motion *past = ready_pair(engine, 1);
    mb_layer_model_pass pass = {engine->fields.layer_raw, model, past ? past->layer_model[direction] : NULL,
                                (int)layer_cells(&engine->geo), engine->params.layer.min_share};
    engine->backend.ops->layer_model(engine->backend.self, &pass);
}

static void search_layer(mb_engine *engine, int direction, mb_buffer *model)
{
    const geometry *g = &engine->geo;
    const pair_motion *past = ready_pair(engine, 1);
    mb_buffer *guide = past && engine->params.layer.pull > 0.0f ? past->layer_model[direction] : NULL;
    int previous = ring_index(engine, 1);
    int newest = engine->pair.newest;
    mb_layer_search_pass pass = {
        level_image(engine, direction ? newest : previous, top_level(g)),
        level_image(engine, direction ? previous : newest, top_level(g)), guide, engine->fields.layer_raw,
        layer_grid(g), engine->params.layer.pull,
    };
    engine->backend.ops->layer_search(engine->backend.self, &pass);
    fit_model(engine, direction, model);
}

static void smooth_layer(mb_engine *engine, mb_buffer *model, mb_buffer *field)
{
    mb_buffer *source = NULL;
    mb_buffer *target = MB_LAYER_SMOOTH_PASSES % 2 ? field : engine->fields.layer_scratch;
    for (int i = 0; i < MB_LAYER_SMOOTH_PASSES; i++) {
        mb_layer_smooth_pass pass = {
            engine->fields.layer_raw, model, source, target, layer_grid(&engine->geo), engine->params.layer.min_share,
        };
        engine->backend.ops->layer_smooth(engine->backend.self, &pass);
        source = target;
        target = target == field ? engine->fields.layer_scratch : field;
    }
}

static void estimate_direction(mb_engine *engine, int direction)
{
    pair_motion *m = pair_at(engine, 0);
    search_layer(engine, direction, m->layer_model[direction]);
    smooth_layer(engine, m->layer_model[direction], m->layer_field[direction]);
}

static void compare_directions(mb_engine *engine)
{
    pair_motion *m = pair_at(engine, 0);
    pair_motion *past = ready_pair(engine, 1);
    mb_layer_pair_pass pass = {
        {m->layer_model[0], m->layer_model[1]},
        {past ? past->layer_model[0] : NULL, past ? past->layer_model[1] : NULL},
        {m->layer_field[0], m->layer_field[1]},
        {past ? past->layer_field[0] : NULL, past ? past->layer_field[1] : NULL},
        (int)layer_cells(&engine->geo),
        engine->params.layer.min_share,
    };
    engine->backend.ops->layer_pair(engine->backend.self, &pass);
}

static mb_layer_side side_of(const mb_engine *engine, int slot, const pair_motion *m, int direction)
{
    if (!m) {
        return (mb_layer_side){0};
    }
    return (mb_layer_side){level_image(engine, slot, mask_level(&engine->geo)), m->layer_field[direction],
                           m->layer_model[direction], m->stats};
}

static void blur_mask(mb_engine *engine, mb_layer_gate gate, mb_buffer *source, mb_buffer *target, bool decide)
{
    mb_grid mask = engine->geo.grids[mask_level(&engine->geo)];
    mb_layer_blur_pass pass = {gate, source, target, mask.width, mask.height, decide};
    engine->backend.ops->layer_blur(engine->backend.self, &pass);
}

static void mask_frame(mb_engine *engine, int slot, mb_layer_side before, mb_layer_side after, mb_buffer *trigger)
{
    const geometry *g = &engine->geo;
    mb_grid mask = g->grids[mask_level(g)];
    mb_layer_gate gate = {{before.model, after.model}, trigger};
    mb_layer_evidence_pass pass = {
        trigger, level_image(engine, slot, mask_level(g)), level_image(engine, slot, detail_level(g)), {before, after},
        engine->fields.mask_evidence, layer_grid(g), mask.width, mask.height, 1 << (mask_level(g) - detail_level(g)),
    };
    engine->backend.ops->layer_evidence(engine->backend.self, &pass);
    blur_mask(engine, gate, engine->fields.mask_evidence, engine->fields.mask_scratch, true);
    blur_mask(engine, gate, engine->fields.mask_scratch, engine->frames[slot].layer_mask, false);
}

void estimate_layers(mb_engine *engine)
{
    if (!layers_enabled(engine)) {
        return;
    }
    estimate_direction(engine, 0);
    estimate_direction(engine, 1);
    compare_directions(engine);
    const pair_motion *current = pair_at(engine, 0);
    const pair_motion *past = ready_pair(engine, 1);
    int newest = engine->pair.newest;
    int previous = ring_index(engine, 1);
    mask_frame(engine, previous, side_of(engine, ring_index(engine, 2), past, 1), side_of(engine, newest, current, 0),
               NULL);
    if (past) {
        mask_frame(engine, ring_index(engine, 2), side_of(engine, ring_index(engine, 3), ready_pair(engine, 2), 1),
                   side_of(engine, previous, past, 0), past->layer_model[0]);
    }
}

static void guard_output(mb_engine *engine, const pair_choice *pair, const mb_warp *warp)
{
    const geometry *g = &engine->geo;
    mb_grid mask = g->grids[mask_level(g)];
    mb_layer_guard_pass pass = {
        warp->frames.a.luma, warp->frames.b.luma, warp->output.luma, warp->stats,
        {pair->current->layer_model[0], pair->current->layer_model[1]},
        {engine->frames[pair->a].layer_mask, engine->frames[pair->b].layer_mask}, engine->sel.layer_guard, mask.width,
        mask.height, warp->width, warp->height, 1 << mask_level(g), warp->t, geometry_luma_scale(g),
    };
    engine->backend.ops->layer_guard(engine->backend.self, &pass);
}

void composite_layers(mb_engine *engine, const pair_choice *pair, mb_warp warp)
{
    if (!layers_enabled(engine)) {
        return;
    }
    const geometry *g = &engine->geo;
    const pair_motion *m = pair->current;
    mb_grid mask = g->grids[mask_level(g)];
    if (pair->current == pair_at(engine, 0)) {
        mb_layer_side none = {0};
        mask_frame(engine, pair->b, side_of(engine, pair->a, pair->current, 1), none, NULL);
    }
    guard_output(engine, pair, &warp);
    mb_layer_composite_pass pass = {
        warp, {m->layer_field[0], m->layer_field[1]}, {m->layer_model[0], m->layer_model[1]},
        {engine->frames[pair->a].layer_mask, engine->frames[pair->b].layer_mask}, engine->sel.layer_guard,
        layer_grid(g), mask.width, mask.height, 1 << top_level(g), 1 << mask_level(g),
    };
    engine->backend.ops->layer_composite(engine->backend.self, &pass);
}
