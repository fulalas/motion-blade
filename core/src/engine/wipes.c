#include "engine/wipes.h"

#include "engine/geometry.h"
#include "engine/motion.h"

enum { NEWEST_PAIR = 0, WIPED_PAIR = 1 };

static bool has_wipe_level(const geometry *g)
{
    return g->level_count - g->base_level >= 2;
}

static int wipe_level(const geometry *g)
{
    return has_wipe_level(g) ? g->base_level + 1 : g->base_level;
}

static mb_wipe_grid wipe_grid(const geometry *g)
{
    mb_grid level = g->grids[wipe_level(g)];
    return (mb_wipe_grid){(level.width + MB_WIPE_CELL - 1) / MB_WIPE_CELL, (level.height + MB_WIPE_CELL - 1) / MB_WIPE_CELL,
                          level.width, level.height};
}

size_t wipe_cells(const geometry *g)
{
    mb_wipe_grid grid = wipe_grid(g);
    return (size_t)grid.cells_x * (size_t)grid.cells_y;
}

static bool fits_lines(const geometry *g)
{
    mb_wipe_grid grid = wipe_grid(g);
    return grid.cells_x <= MB_WIPE_MAX_LINE && grid.cells_y <= MB_WIPE_MAX_LINE;
}

static bool wipes_enabled(const mb_engine *engine)
{
    return engine->params.wipe.enabled && has_wipe_level(&engine->geo) && fits_lines(&engine->geo);
}

static void close_gate(mb_engine *engine, pair_motion *m)
{
    engine->backend.ops->fill_buffer(engine->backend.self, m->wipe_gate, 0);
}

static void find_seeds(mb_engine *engine, pair_motion *wiped, mb_image *const frames[4])
{
    mb_wipe_seeds_pass pass = {
        {frames[0], frames[1], frames[2], frames[3]}, engine->fields.wipe_seeds, wiped->wipe_gate, wipe_grid(&engine->geo),
    };
    engine->backend.ops->wipe_seeds(engine->backend.self, &pass);
}

static void index_seeds(mb_engine *engine, pair_motion *wiped)
{
    const mb_backend *b = &engine->backend;
    mb_wipe_index_pass pass = {engine->fields.wipe_seeds, wiped->wipe_gate, engine->fields.wipe_nearest, wipe_grid(&engine->geo)};
    b->ops->wipe_index(b->self, &pass);
}

static void assign_fronts(mb_engine *engine, pair_motion *wiped, mb_image *const frames[4])
{
    mb_wipe_assign_pass pass = {
        {frames[0], frames[1], frames[2], frames[3]}, engine->fields.wipe_seeds, wiped->wipe_gate,
        engine->fields.wipe_nearest, wiped->wipe_fronts, wipe_grid(&engine->geo),
    };
    engine->backend.ops->wipe_assign(engine->backend.self, &pass);
}

void estimate_wipes(mb_engine *engine)
{
    if (!wipes_enabled(engine)) {
        return;
    }
    close_gate(engine, pair_at(engine, NEWEST_PAIR));
    pair_motion *wiped = ready_pair(engine, WIPED_PAIR);
    if (engine->pair.frame_count < FRAME_RING || !wiped) {
        return;
    }
    int level = wipe_level(&engine->geo);
    mb_image *frames[4];
    for (int k = 0; k < 4; k++) {
        frames[k] = level_image(engine, ring_index(engine, 3 - k), level);
    }
    find_seeds(engine, wiped, frames);
    index_seeds(engine, wiped);
    assign_fronts(engine, wiped, frames);
}

void composite_wipes(mb_engine *engine, const pair_choice *pair, mb_warp warp)
{
    if (!wipes_enabled(engine)) {
        return;
    }
    mb_wipe_composite_pass pass = {
        warp, pair->current->wipe_fronts, pair->current->wipe_gate, wipe_grid(&engine->geo), 1 << wipe_level(&engine->geo),
    };
    engine->backend.ops->wipe_composite(engine->backend.self, &pass);
}
