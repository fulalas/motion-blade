#include "layer/common.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { TAPS = MB_LAYER_WINDOW_TAPS * MB_LAYER_WINDOW_TAPS, REACH = MB_LAYER_WINDOW_TAPS - 1 };

typedef struct search_cell {
    const ref_texture *target;
    float origin[TAPS];
    vec2 center;
    vec2 model;
    float pull;
} search_cell;

typedef struct candidate_cost {
    float sad;
    bool complete;
} candidate_cost;

typedef struct search_best {
    float cost;
    float sad;
    vec2 vector;
    bool found;
} search_best;

static vec2 tap_offset(int k)
{
    return v2((float)(2 * (k % MB_LAYER_WINDOW_TAPS) - REACH), (float)(2 * (k / MB_LAYER_WINDOW_TAPS) - REACH));
}

static float corner_sample(const ref_texture *t, vec2 position)
{
    return ref_sample(t, position.x / (float)t->width, position.y / (float)t->height, 0);
}

static bool inside(const ref_texture *t, vec2 p)
{
    return p.x >= 1.0f && p.x <= (float)(t->width - 1) && p.y >= 1.0f && p.y <= (float)(t->height - 1);
}

static candidate_cost cost_of(const search_cell *cell, vec2 vector)
{
    float sum = 0.0f;
    float count = 0.0f;
    for (int k = 0; k < TAPS; k++) {
        vec2 p = v2_add(v2_add(cell->center, tap_offset(k)), vector);
        if (inside(cell->target, p)) {
            sum += fabsf(cell->origin[k] - corner_sample(cell->target, p));
            count += 1.0f;
        }
    }
    return (candidate_cost){sum / fmaxf(count, 1e-6f) * 255.0f, count >= (float)TAPS - 0.5f};
}

static void consider(const search_cell *cell, search_best *best, vec2 vector)
{
    candidate_cost c = cost_of(cell, vector);
    float cost = c.sad + cell->pull * v2_l1(v2_sub(vector, cell->model));
    if (c.complete && cost < best->cost) {
        *best = (search_best){cost, c.sad, vector, true};
    }
}

static void search_coarse(const search_cell *cell, search_best *best)
{
    for (int row = 0; row < MB_LAYER_ROWS; row++) {
        for (int column = 0; column < MB_LAYER_COLUMNS; column++) {
            consider(cell, best, v2((float)(column * MB_LAYER_STEP - MB_LAYER_RANGE_X),
                                    (float)(row * MB_LAYER_STEP - MB_LAYER_RANGE_Y)));
        }
    }
}

static void search_fine(const search_cell *cell, search_best *best)
{
    vec2 base = best->vector;
    for (int k = 0; k < 9 && best->found; k++) {
        if (k != 4) {
            consider(cell, best, v2_add(base, v2((float)(k % 3 - 1), (float)(k / 3 - 1))));
        }
    }
}

typedef struct still_cell {
    float sad;
    float gain;
} still_cell;

static still_cell measure_still(const search_cell *cell)
{
    float sum = 0.0f;
    float count = 0.0f;
    float own = 0.0f;
    float other = 0.0f;
    for (int k = 0; k < TAPS; k++) {
        vec2 p = v2_add(cell->center, tap_offset(k));
        if (inside(cell->target, p)) {
            float there = corner_sample(cell->target, p);
            sum += fabsf(cell->origin[k] - there);
            count += 1.0f;
            own += cell->origin[k];
            other += there;
        }
    }
    return (still_cell){sum / fmaxf(count, 1e-6f) * 255.0f, other / fmaxf(own, 1e-4f)};
}

static vec4 raw_vector(const search_best *best, still_cell still)
{
    float zero = still.sad;
    bool moving = best->found && best->sad < (float)MB_LAYER_MAX_RATIO * zero &&
                  zero - best->sad > (float)MB_LAYER_MIN_GAIN && v2_l1(best->vector) >= (float)MB_LAYER_MIN_LENGTH;
    float weight = moving ? clampf(zero - best->sad, 0.0f, (float)MB_LAYER_MAX_GAIN) : 0.0f;
    return (vec4){best->vector.x, best->vector.y, weight, still.gain};
}

void ref_layer_search(const mb_layer_search_pass *pass)
{
    mb_layer_grid g = pass->grid;
    vec4 *raw = vec4s(pass->raw);
    search_cell cell = {.target = &pass->target->texture};
    if (pass->model) {
        cell.model = v4_xy(vec4s(pass->model)[0]);
        cell.pull = pass->pull;
    }
    for (int id = 0; id < g.cells_x * g.cells_y; id++) {
        cell.center = v2((float)((id % g.cells_x) * MB_LAYER_CELL + MB_LAYER_CELL / 2),
                         (float)((id / g.cells_x) * MB_LAYER_CELL + MB_LAYER_CELL / 2));
        for (int k = 0; k < TAPS; k++) {
            cell.origin[k] = corner_sample(&pass->origin->texture, v2_add(cell.center, tap_offset(k)));
        }
        search_best best = {1e30f, 0.0f, v2(0.0f, 0.0f), false};
        search_coarse(&cell, &best);
        search_fine(&cell, &best);
        raw[id] = raw_vector(&best, measure_still(&cell));
    }
}
