#include "layer/common.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { LANES = MB_LAYER_MODEL_LANES, ROUNDS = 3 };

typedef struct cell_query {
    const vec4 *raw;
    const float *distinct;
    mb_layer_grid grid;
    vec2 model;
    float tolerance;
    float mean_gain;
    bool steady;
} cell_query;

typedef vec4 (*cell_term)(int cell, const cell_query *q);

static vec4 add4(vec4 a, vec4 b)
{
    return (vec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

static int cell_count(mb_layer_grid grid)
{
    return grid.cells_x * grid.cells_y;
}

static vec4 lane_total(cell_term term, const cell_query *q)
{
    vec4 total = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int lane = 0; lane < LANES; lane++) {
        vec4 part = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = lane; i < cell_count(q->grid); i += LANES) {
            part = add4(part, term(i, q));
        }
        total = add4(total, part);
    }
    return total;
}

static bool is_moving(vec4 raw)
{
    return raw.z > 0.0f;
}

static vec4 robust_term(int cell, const cell_query *q)
{
    vec4 raw = q->raw[cell];
    float r = v2_l1(v2_sub(v4_xy(raw), q->model)) / (3.0f * q->tolerance);
    float w = clampf(1.0f - r * r, 0.0f, 1.0f);
    w = raw.z * w * w;
    return (vec4){w * raw.x, w * raw.y, w, 0.0f};
}

static vec2 refit(cell_query *q, float tolerance)
{
    q->tolerance = tolerance;
    vec4 t = lane_total(robust_term, q);
    return t.z < 1e-6f ? q->model : v2(t.x / t.z, t.y / t.z);
}

static float agreeing_weight(vec4 raw, vec2 model)
{
    float limit = (float)MB_LAYER_CONSENSUS_REL * v2_l1(model) + (float)MB_LAYER_CONSENSUS_ABS;
    return v2_l1(v2_sub(v4_xy(raw), model)) <= limit ? raw.z : 0.0f;
}

static bool window_inside(mb_layer_grid g, int cx, int cy, vec2 model)
{
    float reach = (float)(MB_LAYER_WINDOW_TAPS - 1);
    float x = (float)(cx * MB_LAYER_CELL + MB_LAYER_CELL / 2) + model.x;
    float y = (float)(cy * MB_LAYER_CELL + MB_LAYER_CELL / 2) + model.y;
    return x - reach >= 1.0f && x + reach <= (float)(g.width - 1) && y - reach >= 1.0f && y + reach <= (float)(g.height - 1);
}

static bool is_covered(const cell_query *q, int cx, int cy)
{
    int steps = (int)ceilf(fmaxf(fabsf(q->model.x), fabsf(q->model.y)) / (float)MB_LAYER_CELL);
    for (int s = 1; s <= steps; s++) {
        float part = (float)s / (float)(steps * MB_LAYER_CELL);
        int x = cx - (int)floorf(q->model.x * part + 0.5f);
        int y = cy - (int)floorf(q->model.y * part + 0.5f);
        if (layer_cell_inside(q->grid, x, y) && agreeing_weight(q->raw[y * q->grid.cells_x + x], q->model) > 0.0f) {
            return true;
        }
    }
    return false;
}

static vec4 summary_term(int cell, const cell_query *q)
{
    vec4 raw = q->raw[cell];
    return (vec4){is_moving(raw) ? 1.0f : 0.0f, fabsf(raw.w - 1.0f) <= (float)MB_LAYER_LIGHT_STEADY ? 1.0f : 0.0f, 0.0f,
                  raw.w};
}

static bool counts_in_vote(const cell_query *q, int cell, float agreeing)
{
    int cx = cell % q->grid.cells_x;
    int cy = cell / q->grid.cells_x;
    return !q->steady || (window_inside(q->grid, cx, cy, q->model) && (agreeing > 0.0f || !is_covered(q, cx, cy)));
}

static vec4 vote_term(int cell, const cell_query *q)
{
    vec4 raw = q->raw[cell];
    float agreeing = agreeing_weight(raw, q->model);
    bool counted = counts_in_vote(q, cell, agreeing);
    return (vec4){fabsf(raw.w - q->mean_gain) <= (float)MB_LAYER_LIGHT_BAND ? 1.0f : 0.0f, counted ? agreeing : 0.0f,
                  counted ? raw.z : 0.0f, agreeing > 0.0f ? q->distinct[cell] : 0.0f};
}

static vec2 best_supported(const vec4 *raw, int cells)
{
    vec2 best = v2(0.0f, 0.0f);
    float best_support = 0.0f;
    for (int lane = 0; lane < LANES; lane++) {
        vec4 candidate = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = lane; i < cells; i += LANES) {
            if (raw[i].z > candidate.z) {
                candidate = raw[i];
            }
        }
        float support = 0.0f;
        for (int i = 0; i < cells; i++) {
            support += agreeing_weight(raw[i], v4_xy(candidate));
        }
        if (support > best_support) {
            best_support = support;
            best = v4_xy(candidate);
        }
    }
    return best;
}

static bool light_changed(float mean, float steady_share, float band_share)
{
    return fabsf(mean - 1.0f) > (float)MB_LAYER_LIGHT_STEADY && steady_share < (float)MB_LAYER_LIGHT_SHARE &&
           band_share >= (float)MB_LAYER_LIGHT_SHARE;
}

static bool inside_search(vec2 model)
{
    float margin = (float)MB_LAYER_SEARCH_MARGIN;
    return fabsf(model.x) <= (float)MB_LAYER_RANGE_X - margin && fabsf(model.y) <= (float)MB_LAYER_RANGE_Y - margin;
}

static bool is_layer(const mb_layer_model_pass *pass, vec2 model, float share, float distinct_share, float agreement)
{
    bool kept = layer_is_active(pass->previous);
    float measured_share = kept ? share : distinct_share;
    float min_length = kept ? (float)MB_LAYER_KEEP_MIN_LENGTH : (float)MB_LAYER_MODEL_MIN_LENGTH;
    float min_share = kept ? pass->min_share * (float)MB_LAYER_KEEP_SHARE : pass->min_share;
    float min_agreement = kept ? (float)MB_LAYER_KEEP_CONSENSUS : (float)MB_LAYER_MIN_CONSENSUS;
    return measured_share >= min_share && v2_l1(model) >= min_length && agreement >= min_agreement &&
           inside_search(model);
}

void ref_layer_model(const mb_layer_model_pass *pass)
{
    static const float TOLERANCES[ROUNDS] = {4.0f, 2.0f, 2.0f};
    cell_query q = {.raw = vec4s(pass->raw), .distinct = floats(pass->distinct), .grid = pass->grid};
    int cells = cell_count(q.grid);
    vec2 model = best_supported(q.raw, cells);
    q.model = model;
    for (int round = 0; round < ROUNDS; round++) {
        q.model = refit(&q, TOLERANCES[round]);
    }
    model = q.model;
    vec4 t = lane_total(summary_term, &q);
    float steady_share = t.y / (float)cells;
    q.mean_gain = t.w / (float)cells;
    q.steady = steady_share >= (float)MB_LAYER_VOTE_STEADY;
    vec4 vote = lane_total(vote_term, &q);
    float share = t.x / (float)cells;
    float distinct_share = vote.w / (float)cells;
    bool active = is_layer(pass, model, share, distinct_share, vote.y / fmaxf(vote.z, 1e-6f)) &&
                  !light_changed(q.mean_gain, steady_share, vote.x / (float)cells);
    vec4s(pass->model)[0] = (vec4){model.x, model.y, active ? 1.0f : 0.0f, share};
}

static bool directions_agree(vec4 forward, vec4 backward)
{
    float longest = fmaxf(v2_l1(v4_xy(forward)), v2_l1(v4_xy(backward)));
    float apart = v2_l1(v2_add(v4_xy(forward), v4_xy(backward)));
    return apart <= (float)MB_LAYER_PAIR_REL * longest + (float)MB_LAYER_PAIR_ABS;
}

static bool is_measured(vec4 model)
{
    return fabsf(model.z - 1.0f) < 0.125f;
}

static bool big_enough_to_keep(vec4 model, float min_share)
{
    return model.w >= min_share * (float)MB_LAYER_KEEP_SHARE && v2_l1(v4_xy(model)) >= (float)MB_LAYER_KEEP_MIN_LENGTH;
}

static bool continues_into(vec4 past, vec4 current, float min_share)
{
    float apart = v2_l1(v2_sub(v4_xy(past), v4_xy(current)));
    return current.z > 0.5f && big_enough_to_keep(past, min_share) &&
           apart <= (float)MB_LAYER_ADOPT_REL * v2_l1(v4_xy(current)) + (float)MB_LAYER_ADOPT_ABS;
}

static bool measured_layer(vec4 forward, vec4 backward)
{
    return is_measured(forward) && is_measured(backward);
}

static bool nearly_measured(vec4 measured, vec4 other, float min_share)
{
    return is_measured(measured) && big_enough_to_keep(other, min_share);
}

static bool entering_layer(vec4 forward, vec4 backward, float min_share)
{
    return inside_search(v4_xy(forward)) && inside_search(v4_xy(backward)) &&
           (nearly_measured(forward, backward, min_share) || nearly_measured(backward, forward, min_share));
}

static bool no_layer(vec4 forward, vec4 backward)
{
    return forward.z <= 0.5f && backward.z <= 0.5f;
}

static ivec2 cell_shift(vec2 motion)
{
    return iv2((int)floorf(motion.x / (float)MB_LAYER_CELL + 0.5f), (int)floorf(motion.y / (float)MB_LAYER_CELL + 0.5f));
}

static void borrow(vec4 *const to[2], mb_buffer *const to_fields[2], vec4 *const from[2],
                   mb_buffer *const from_fields[2], mb_layer_grid grid, ivec2 shift, float state)
{
    for (int d = 0; d < 2; d++) {
        for (int i = 0; i < cell_count(grid); i++) {
            int source = layer_cell_index(grid, i % grid.cells_x + shift.x, i / grid.cells_x + shift.y);
            vec4s(to_fields[d])[i] = vec4s(from_fields[d])[source];
        }
        *to[d] = (vec4){from[d]->x, from[d]->y, state, from[d]->w};
    }
}

static vec2 forward_motion(vec4 forward, vec4 backward)
{
    return is_measured(forward) ? v4_xy(forward) : v2_neg(v4_xy(backward));
}

static bool visible_before(const vec4 *field, mb_layer_grid g, ivec2 shift)
{
    float inside = 0.0f;
    float total = 0.0f;
    for (int i = 0; i < cell_count(g); i++) {
        bool layer = field[i].z > 0.5f;
        total += layer ? 1.0f : 0.0f;
        inside += layer && layer_cell_inside(g, i % g.cells_x - shift.x, i / g.cells_x - shift.y) ? 1.0f : 0.0f;
    }
    return total > 0.0f && inside >= (float)MB_LAYER_LEND_VISIBLE * total;
}

typedef struct cell_cluster {
    float blocks;
    float inliers;
    vec2 sum;
    float squares;
} cell_cluster;

static cell_cluster cluster_in(const mb_layer_pair_pass *pass, int direction, int cell)
{
    const vec4 *vectors = vec4s(pass->vectors[direction]);
    vec2 expected = v4_xy(vec4s(pass->fields[direction])[cell]);
    float limit = (float)MB_LAYER_TRACK_REL * v2_l1(expected) + (float)MB_LAYER_TRACK_ABS;
    int x0 = cell % pass->grid.cells_x * pass->cell_blocks;
    int y0 = cell / pass->grid.cells_x * pass->cell_blocks;
    cell_cluster c = {0.0f, 0.0f, v2(0.0f, 0.0f), 0.0f};
    for (int y = y0; y < y0 + pass->cell_blocks && y < pass->blocks.blocks_y; y++) {
        for (int x = x0; x < x0 + pass->cell_blocks && x < pass->blocks.blocks_x; x++) {
            vec2 apart = v2_sub(v2_scale(v4_xy(vectors[y * pass->blocks.blocks_x + x]), pass->vector_scale), expected);
            bool inlier = v2_l1(apart) <= limit;
            c.blocks += 1.0f;
            c.inliers += inlier ? 1.0f : 0.0f;
            c.sum = v2_add(c.sum, inlier ? apart : v2(0.0f, 0.0f));
            c.squares += inlier ? apart.x * apart.x + apart.y * apart.y : 0.0f;
        }
    }
    return c;
}

static bool is_tight(cell_cluster c, float vector_scale)
{
    if (c.inliers < 0.5f || c.inliers < (float)MB_LAYER_TRACK_INLIERS * c.blocks) {
        return false;
    }
    vec2 mean = v2_scale(c.sum, 1.0f / c.inliers);
    float spread = (float)MB_LAYER_TRACK_SPREAD * vector_scale;
    return c.squares / c.inliers - (mean.x * mean.x + mean.y * mean.y) <= spread * spread;
}

static vec2 tracked_cells(const mb_layer_pair_pass *pass, int direction)
{
    const vec4 *field = vec4s(pass->fields[direction]);
    vec2 counts = v2(0.0f, 0.0f);
    if (!layer_is_active(pass->models[direction])) {
        return counts;
    }
    for (int i = 0; i < cell_count(pass->grid); i++) {
        if (field[i].z > 0.5f) {
            counts.x += 1.0f;
            counts.y += is_tight(cluster_in(pass, direction, i), pass->vector_scale) ? 1.0f : 0.0f;
        }
    }
    return counts;
}

static bool already_tracked(const mb_layer_pair_pass *pass)
{
    vec2 counts = v2_add(tracked_cells(pass, 0), tracked_cells(pass, 1));
    return counts.x > 0.0f && counts.y >= (float)MB_LAYER_TRACK_SHARE * counts.x;
}

void ref_layer_pair(const mb_layer_pair_pass *pass)
{
    vec4 *current[2] = {vec4s(pass->models[0]), vec4s(pass->models[1])};
    bool tracked = already_tracked(pass);
    if (tracked || !directions_agree(*current[0], *current[1])) {
        current[0]->z = 0.0f;
        current[1]->z = 0.0f;
    }
    if (!pass->past[0]) {
        return;
    }
    vec4 *past[2] = {vec4s(pass->past[0]), vec4s(pass->past[1])};
    if (directions_agree(*past[0], *past[1])) {
        for (int d = 0; d < 2; d++) {
            if (past[d]->z <= 0.5f && continues_into(*past[d], *current[d], pass->min_share)) {
                past[d]->z = (float)MB_LAYER_ADOPTED_STATE;
            }
        }
    }
    ivec2 shift = cell_shift(forward_motion(*current[0], *current[1]));
    if (!tracked && no_layer(*current[0], *current[1]) && measured_layer(*past[0], *past[1])) {
        borrow(current, pass->fields, past, pass->past_fields, pass->grid, iv2(0, 0), (float)MB_LAYER_BORROWED);
    } else if (no_layer(*past[0], *past[1]) && entering_layer(*current[0], *current[1], pass->min_share) &&
               visible_before(vec4s(pass->fields[0]), pass->grid, shift)) {
        borrow(past, pass->past_fields, current, pass->fields, pass->grid, shift, (float)MB_LAYER_ADOPTED_STATE);
    }
}

static bool is_inlier(vec4 raw, vec2 expected)
{
    float limit = fmaxf((float)MB_LAYER_INLIER_REL * v2_l1(expected), (float)MB_LAYER_INLIER_ABS);
    return is_moving(raw) && v2_l1(v2_sub(v4_xy(raw), expected)) < limit;
}

static vec2 smoothed_vector(const mb_layer_smooth_pass *pass, const layer_kernel *k, int cx, int cy, vec2 model)
{
    const vec4 *raw = vec4s(pass->raw);
    const vec4 *previous = pass->previous ? vec4s(pass->previous) : NULL;
    vec2 sum = v2(0.0f, 0.0f);
    float total = 0.0f;
    for (int dy = -k->radius; dy <= k->radius; dy++) {
        for (int dx = -k->radius; dx <= k->radius; dx++) {
            int j = layer_cell_index(pass->grid, cx + dx, cy + dy);
            vec2 expected = previous ? v4_xy(previous[j]) : model;
            if (is_inlier(raw[j], expected)) {
                float w = raw[j].z * k->weights[dy + k->radius] * k->weights[dx + k->radius];
                sum = v2_add(sum, v2_scale(v4_xy(raw[j]), w));
                total += w;
            }
        }
    }
    return v2_scale(v2_add(sum, model), 1.0f / (total + 1.0f));
}

static vec2 prior_at(const mb_layer_smooth_pass *pass, const layer_kernel *k, int cx, int cy, vec2 model)
{
    const vec4 *raw = vec4s(pass->raw);
    float moving = 0.0f;
    float known = 0.0f;
    for (int dy = -k->radius; dy <= k->radius; dy++) {
        for (int dx = -k->radius; dx <= k->radius; dx++) {
            int x = clampi(cx + dx, 0, pass->grid.cells_x - 1);
            int y = clampi(cy + dy, 0, pass->grid.cells_y - 1);
            if (window_inside(pass->grid, x, y, model)) {
                float w = k->weights[dy + k->radius] * k->weights[dx + k->radius];
                moving += is_moving(raw[layer_cell_index(pass->grid, x, y)]) ? w : 0.0f;
                known += w;
            }
        }
    }
    float share = moving / fmaxf(known, 1e-6f);
    return v2(clampf((share - (float)MB_LAYER_PRIOR_LO) / (float)MB_LAYER_PRIOR_SPAN, 0.0f, 1.0f), known);
}

static bool may_become_layer(vec4 model, float min_share)
{
    return model.z > 0.5f || model.w >= min_share * (float)MB_LAYER_KEEP_SHARE;
}

void ref_layer_smooth(const mb_layer_smooth_pass *pass)
{
    if (!may_become_layer(vec4s(pass->model)[0], pass->min_share)) {
        return;
    }
    mb_layer_grid g = pass->grid;
    layer_kernel smooth = layer_gauss((float)MB_LAYER_SMOOTH_SIGMA, MB_LAYER_SMOOTH_RADIUS);
    layer_kernel prior = layer_gauss((float)MB_LAYER_PRIOR_SIGMA, MB_LAYER_PRIOR_RADIUS);
    vec2 model = v4_xy(vec4s(pass->model)[0]);
    vec4 *field = vec4s(pass->field);
    for (int id = 0; id < g.cells_x * g.cells_y; id++) {
        int cx = id % g.cells_x;
        int cy = id / g.cells_x;
        vec2 v = smoothed_vector(pass, &smooth, cx, cy, model);
        vec2 p = prior_at(pass, &prior, cx, cy, model);
        field[id] = (vec4){v.x, v.y, p.x, p.y};
    }
}
