#include "layer/common.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { LANES = MB_LAYER_MODEL_LANES, ROUNDS = 3 };

typedef struct cell_query {
    vec2 model;
    float tolerance;
    float center;
    float band;
} cell_query;

typedef vec4 (*cell_term)(vec4 raw, const cell_query *q);

static vec4 add4(vec4 a, vec4 b)
{
    return (vec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

static vec4 lane_total(const vec4 *raw, int cells, cell_term term, const cell_query *q)
{
    vec4 total = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int lane = 0; lane < LANES; lane++) {
        vec4 part = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = lane; i < cells; i += LANES) {
            part = add4(part, term(raw[i], q));
        }
        total = add4(total, part);
    }
    return total;
}

static bool is_moving(vec4 raw)
{
    return raw.z > 0.0f;
}

static vec4 robust_term(vec4 raw, const cell_query *q)
{
    float r = v2_l1(v2_sub(v4_xy(raw), q->model)) / (3.0f * q->tolerance);
    float w = clampf(1.0f - r * r, 0.0f, 1.0f);
    w = raw.z * w * w;
    return (vec4){w * raw.x, w * raw.y, w, 0.0f};
}

static vec2 refit(const vec4 *raw, int cells, vec2 model, float tolerance)
{
    cell_query q = {model, tolerance, 0.0f, 0.0f};
    vec4 t = lane_total(raw, cells, robust_term, &q);
    return t.z < 1e-6f ? model : v2(t.x / t.z, t.y / t.z);
}

static float agreeing_weight(vec4 raw, vec2 model)
{
    float limit = (float)MB_LAYER_CONSENSUS_REL * v2_l1(model) + (float)MB_LAYER_CONSENSUS_ABS;
    return v2_l1(v2_sub(v4_xy(raw), model)) <= limit ? raw.z : 0.0f;
}

static vec4 summary_term(vec4 raw, const cell_query *q)
{
    return (vec4){is_moving(raw) ? 1.0f : 0.0f, agreeing_weight(raw, q->model), raw.z, raw.w};
}

static vec4 gain_term(vec4 raw, const cell_query *q)
{
    return (vec4){fabsf(raw.w - q->center) <= q->band ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
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

static float gain_share(const vec4 *raw, int cells, float center, float band)
{
    cell_query q = {v2(0.0f, 0.0f), 0.0f, center, band};
    return lane_total(raw, cells, gain_term, &q).x / (float)cells;
}

static bool light_changed(const vec4 *raw, int cells, float mean)
{
    return fabsf(mean - 1.0f) > (float)MB_LAYER_LIGHT_STEADY &&
           gain_share(raw, cells, 1.0f, (float)MB_LAYER_LIGHT_STEADY) < (float)MB_LAYER_LIGHT_SHARE &&
           gain_share(raw, cells, mean, (float)MB_LAYER_LIGHT_BAND) >= (float)MB_LAYER_LIGHT_SHARE;
}

static bool is_layer(const mb_layer_model_pass *pass, vec2 model, float share, float agreement)
{
    bool kept = layer_is_active(pass->previous);
    float min_length = kept ? (float)MB_LAYER_KEEP_MIN_LENGTH : (float)MB_LAYER_MODEL_MIN_LENGTH;
    float min_share = kept ? pass->min_share * (float)MB_LAYER_KEEP_SHARE : pass->min_share;
    float min_agreement = kept ? (float)MB_LAYER_KEEP_CONSENSUS : (float)MB_LAYER_MIN_CONSENSUS;
    return share >= min_share && v2_l1(model) >= min_length && agreement >= min_agreement;
}

void ref_layer_model(const mb_layer_model_pass *pass)
{
    static const float TOLERANCES[ROUNDS] = {4.0f, 2.0f, 2.0f};
    const vec4 *raw = vec4s(pass->raw);
    int cells = pass->cells;
    vec2 model = best_supported(raw, cells);
    for (int round = 0; round < ROUNDS; round++) {
        model = refit(raw, cells, model, TOLERANCES[round]);
    }
    cell_query q = {model, 0.0f, 0.0f, 0.0f};
    vec4 t = lane_total(raw, cells, summary_term, &q);
    float share = t.x / (float)cells;
    bool active = is_layer(pass, model, share, t.y / fmaxf(t.z, 1e-6f)) && !light_changed(raw, cells, t.w / (float)cells);
    vec4s(pass->model)[0] = (vec4){model.x, model.y, active ? 1.0f : 0.0f, share};
}

static bool directions_agree(vec4 forward, vec4 backward)
{
    float longest = fmaxf(v2_l1(v4_xy(forward)), v2_l1(v4_xy(backward)));
    float apart = v2_l1(v2_add(v4_xy(forward), v4_xy(backward)));
    return apart <= (float)MB_LAYER_PAIR_REL * longest + (float)MB_LAYER_PAIR_ABS;
}

static bool continues_into(vec4 past, vec4 current, float min_share)
{
    float apart = v2_l1(v2_sub(v4_xy(past), v4_xy(current)));
    return current.z > 0.5f && past.w >= min_share * (float)MB_LAYER_KEEP_SHARE &&
           v2_l1(v4_xy(past)) >= (float)MB_LAYER_KEEP_MIN_LENGTH &&
           apart <= (float)MB_LAYER_ADOPT_REL * v2_l1(v4_xy(current)) + (float)MB_LAYER_ADOPT_ABS;
}

static bool measured_layer(vec4 forward, vec4 backward)
{
    return fabsf(forward.z - 1.0f) < 0.125f && fabsf(backward.z - 1.0f) < 0.125f;
}

static bool no_layer(vec4 forward, vec4 backward)
{
    return forward.z <= 0.5f && backward.z <= 0.5f;
}

static void borrow(vec4 *const to[2], mb_buffer *const to_fields[2], vec4 *const from[2],
                   mb_buffer *const from_fields[2], int cells, float state)
{
    for (int d = 0; d < 2; d++) {
        for (int i = 0; i < cells; i++) {
            vec4s(to_fields[d])[i] = vec4s(from_fields[d])[i];
        }
        *to[d] = (vec4){from[d]->x, from[d]->y, state, from[d]->w};
    }
}

void ref_layer_pair(const mb_layer_pair_pass *pass)
{
    vec4 *current[2] = {vec4s(pass->models[0]), vec4s(pass->models[1])};
    if (!directions_agree(*current[0], *current[1])) {
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
                past[d]->z = 2.0f;
            }
        }
    }
    if (no_layer(*current[0], *current[1]) && measured_layer(*past[0], *past[1])) {
        borrow(current, pass->fields, past, pass->past_fields, pass->cells, (float)MB_LAYER_BORROWED);
    } else if (no_layer(*past[0], *past[1]) && measured_layer(*current[0], *current[1])) {
        borrow(past, pass->past_fields, current, pass->fields, pass->cells, 2.0f);
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

static bool window_inside(mb_layer_grid g, int cx, int cy, vec2 model)
{
    float reach = (float)(MB_LAYER_WINDOW_TAPS - 1);
    float x = (float)(cx * MB_LAYER_CELL + MB_LAYER_CELL / 2) + model.x;
    float y = (float)(cy * MB_LAYER_CELL + MB_LAYER_CELL / 2) + model.y;
    return x - reach >= 1.0f && x + reach <= (float)(g.width - 1) && y - reach >= 1.0f && y + reach <= (float)(g.height - 1);
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
