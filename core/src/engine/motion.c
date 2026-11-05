#include "engine/motion.h"

#include "engine/geometry.h"

typedef struct direction_frames {
    int origin;
    int target;
} direction_frames;

static void swap_buffers(mb_buffer **a, mb_buffer **b)
{
    mb_buffer *kept = *a;
    *a = *b;
    *b = kept;
}

int ring_index(const mb_engine *engine, int back)
{
    return (engine->pair.newest - back + FRAME_RING) % FRAME_RING;
}

pair_motion *pair_at(const mb_engine *engine, int back)
{
    const motion_fields *f = &engine->fields;
    return (pair_motion *)&f->pairs[(f->newest_pair - back + PAIR_RING) % PAIR_RING];
}

void motion_advance(mb_engine *engine)
{
    engine->fields.newest_pair = (engine->fields.newest_pair + 1) % PAIR_RING;
    pair_at(engine, 0)->ready = false;
}

void motion_forget(mb_engine *engine)
{
    for (int k = 0; k < PAIR_RING; k++) {
        engine->fields.pairs[k].ready = false;
    }
}

mb_image *level_image(const mb_engine *engine, int slot, int level)
{
    if (level == 0 && !engine->geo.normalized_luma) {
        return engine->frames[slot].planes.luma;
    }
    return engine->frames[slot].levels[level];
}

void build_levels(mb_engine *engine, int slot)
{
    const mb_backend *b = &engine->backend;
    const geometry *g = &engine->geo;
    if (g->normalized_luma) {
        mb_normalize_pass pass = {engine->frames[slot].planes.luma, engine->frames[slot].levels[0], g->grids[0].width,
                                  g->grids[0].height, geometry_luma_scale(g)};
        b->ops->normalize(b->self, &pass);
    }
    for (int level = 1; level < g->level_count; level++) {
        mb_grid from = g->grids[level - 1];
        mb_grid to = g->grids[level];
        mb_downsample_pass pass = {level_image(engine, slot, level - 1), engine->frames[slot].levels[level], to.width,
                                   to.height, from.width, from.height};
        b->ops->downsample(b->self, &pass);
    }
}

static direction_frames frames_for(const mb_engine *engine, int direction)
{
    int previous = ring_index(engine, 1);
    int newest = engine->pair.newest;
    return direction ? (direction_frames){newest, previous} : (direction_frames){previous, newest};
}

static mb_buffer **direction_fields(pair_motion *m, int direction)
{
    return direction ? m->backward : m->forward;
}

static mb_buffer **estimate_fields(mb_engine *engine, int direction)
{
    return direction_fields(pair_at(engine, 0), direction);
}

static mb_buffer *temporal_field(mb_engine *engine, int direction, int level)
{
    pair_motion *previous = pair_at(engine, 1);
    return previous->ready ? direction_fields(previous, direction)[level] : NULL;
}

static mb_buffer *current_stats(const mb_engine *engine)
{
    return pair_at(engine, 0)->stats;
}

static mb_search_penalties search_penalties(const mb_search_params *s)
{
    return (mb_search_penalties){
        s->penalty_parent, s->penalty_temporal, s->penalty_random,
        s->penalty_zero, s->penalty_reverse, s->penalty_length,
    };
}

mb_mean_removal mean_removal_of(const mb_search_params *s)
{
    return (mb_mean_removal){s->mean_removal_lo, s->mean_removal_hi};
}

mb_support_tolerance support_tolerance_of(const mb_select_params *s)
{
    return (mb_support_tolerance){s->support_rel, s->support_abs};
}

static void full_search(mb_engine *engine, direction_frames frames, int direction, int level)
{
    const mb_search_params *s = &engine->params.search;
    mb_full_search_pass pass = {
        level_image(engine, frames.origin, level), level_image(engine, frames.target, level),
        estimate_fields(engine, direction)[level], engine->geo.grids[level], s->full_search_radius,
        s->full_search_length_penalty,
    };
    engine->backend.ops->full_search(engine->backend.self, &pass);
}

typedef struct iteration {
    int direction;
    int level;
    int index;
    bool has_estimate;
} iteration;

static void iterate(mb_engine *engine, direction_frames frames, iteration it)
{
    const mb_search_params *s = &engine->params.search;
    const geometry *g = &engine->geo;
    mb_buffer **fields = estimate_fields(engine, it.direction);
    bool top = it.level == g->level_count - 1;
    mb_iterate_pass pass = {
        .origin = level_image(engine, frames.origin, it.level),
        .target = level_image(engine, frames.target, it.level),
        .previous = it.has_estimate ? fields[it.level] : NULL,
        .field = engine->fields.scratch[it.level],
        .parent = top ? NULL : fields[it.level + 1],
        .temporal = temporal_field(engine, it.direction, it.level),
        .reverse = it.direction ? estimate_fields(engine, 0)[it.level] : NULL,
        .history = engine->fields.history,
        .grid = g->grids[it.level],
        .level = it.level,
        .parent_blocks_x = top ? 1 : g->grids[it.level + 1].blocks_x,
        .parent_blocks_y = top ? 1 : g->grids[it.level + 1].blocks_y,
        .seed = (uint32_t)(it.index + it.level * 16 + it.direction * 256),
        .penalties = search_penalties(s),
        .mean_removal = mean_removal_of(s),
        .early_exit = s->early_exit,
    };
    engine->backend.ops->iterate_search(engine->backend.self, &pass);
    swap_buffers(&fields[it.level], &engine->fields.scratch[it.level]);
}

static void measure(mb_engine *engine, direction_frames frames, int direction)
{
    int base = engine->geo.base_level;
    mb_measure_pass pass = {
        level_image(engine, frames.origin, base), level_image(engine, frames.target, base),
        estimate_fields(engine, direction)[base], current_stats(engine), engine->geo.grids[base], direction == 0,
        direction, mean_removal_of(&engine->params.search),
    };
    engine->backend.ops->measure(engine->backend.self, &pass);
}

static void estimate(mb_engine *engine, int direction)
{
    const geometry *g = &engine->geo;
    const mb_search_params *s = &engine->params.search;
    direction_frames frames = frames_for(engine, direction);
    int top = g->level_count - 1;
    for (int level = top; level >= g->base_level; level--) {
        bool has_estimate = level == top;
        if (has_estimate) {
            full_search(engine, frames, direction, level);
        }
        int count = level == g->base_level ? s->base_iterations : s->iterations;
        for (int i = 0; i < count; i++) {
            iterate(engine, frames, (iteration){direction, level, i, has_estimate});
            has_estimate = true;
        }
        if (level == g->base_level) {
            measure(engine, frames, direction);
        }
    }
}

static mb_acceptance acceptance(const mb_repair_params *r)
{
    return (mb_acceptance){r->max_cost_ratio, r->min_cost_gain};
}

static void cross_check(mb_engine *engine, int direction)
{
    const mb_backend *b = &engine->backend;
    const mb_repair_params *r = &engine->params.repair;
    int base = engine->geo.base_level;
    mb_grid grid = engine->geo.grids[base];
    mb_buffer **fields = estimate_fields(engine, direction);
    mb_buffer *other = estimate_fields(engine, !direction)[base];
    direction_frames frames = frames_for(engine, direction);
    b->ops->fill_buffer(b->self, engine->fields.cross_keys, MB_KEY_NONE);
    mb_project_pass projection = {other, engine->fields.cross_keys, grid, 1.0f, 0};
    b->ops->project(b->self, &projection);
    mb_cross_check_pass pass = {
        level_image(engine, frames.origin, base), level_image(engine, frames.target, base), fields[base],
        engine->fields.scratch[base], other, engine->fields.cross_keys, grid, r->cross_penalty,
        engine->params.search.penalty_length, r->cross_min_change, acceptance(r),
    };
    b->ops->cross_check(b->self, &pass);
    swap_buffers(&fields[base], &engine->fields.scratch[base]);
}

static mb_buffer *local_reference(mb_engine *engine, int direction)
{
    int base = engine->geo.base_level;
    if (!engine->params.repair.local_time_check) {
        return NULL;
    }
    return direction ? estimate_fields(engine, 0)[base] : temporal_field(engine, 1, base);
}

static void local_search(mb_engine *engine, int direction)
{
    const mb_repair_params *r = &engine->params.repair;
    const mb_select_params *s = &engine->params.select;
    int base = engine->geo.base_level;
    mb_buffer **fields = estimate_fields(engine, direction);
    direction_frames frames = frames_for(engine, direction);
    mb_local_search_pass pass = {
        .origin = level_image(engine, frames.origin, base),
        .target = level_image(engine, frames.target, base),
        .field = fields[base],
        .repaired = engine->fields.scratch[base],
        .reference = local_reference(engine, direction),
        .stats = current_stats(engine),
        .grid = engine->geo.grids[base],
        .reference_at_landing = direction == 1,
        .length_penalty = engine->params.search.penalty_length,
        .noise_factor = r->local_noise_factor,
        .min_cost = r->local_min_cost,
        .radius = r->local_radius,
        .step = r->local_step,
        .support = support_tolerance_of(s),
        .acceptance = acceptance(r),
    };
    engine->backend.ops->local_search(engine->backend.self, &pass);
    swap_buffers(&fields[base], &engine->fields.scratch[base]);
}

static void decide_cut(mb_engine *engine)
{
    const mb_cut_params *c = &engine->params.cut;
    int blocks = geometry_base_blocks(&engine->geo);
    mb_cut_pass pass = {
        current_stats(engine), engine->fields.history, blocks, (float)stat_error_mean(1.0, blocks),
        c->max_threshold, c->error_ratio, c->zero_mean_ratio, c->zero_mean_min, c->spread_norm, c->spread_min,
    };
    engine->backend.ops->decide_cut(engine->backend.self, &pass);
}

static void clear_statistics(mb_engine *engine)
{
    const mb_backend *b = &engine->backend;
    b->ops->fill_buffer(b->self, current_stats(engine), 0);
    if (engine->pair.history_stale) {
        b->ops->fill_buffer(b->self, engine->fields.history, 0);
    }
    engine->pair.history_stale = false;
}

mb_status run_motion(mb_engine *engine)
{
    mb_status status = engine->backend.ops->begin_work(engine->backend.self);
    if (status != MB_SUCCESS) {
        return status;
    }
    clear_statistics(engine);
    estimate(engine, 0);
    estimate(engine, 1);
    for (int direction = 0; direction < engine->params.repair.cross_check_directions; direction++) {
        cross_check(engine, direction);
    }
    for (int direction = 0; direction < 2 && engine->params.repair.local_search; direction++) {
        local_search(engine, direction);
    }
    decide_cut(engine);
    pair_at(engine, 0)->ready = true;
    return MB_SUCCESS;
}
