#include "compensation/quad.h"

#include "common/blocks.h"
#include "compensation/chroma.h"
#include "compensation/cubic.h"
#include "reference/cpu_resources.h"

static const float SIDE_LEVELS = 7.0f;
static const unsigned SIDE_BITS = 3;
static const unsigned SIDE_MASK = 7;

static ivec2 pixel_of(const quad_state *q, int j)
{
    return iv2(q->q0.x + (j & 1), q->q0.y + (j >> 1));
}

static vec2 at_level(const quad_state *q, ivec2 p)
{
    return v2(((float)p.x + 0.5f) / q->scale - 0.5f, ((float)p.y + 0.5f) / q->scale - 0.5f);
}

void quad_begin(quad_state *q, const mb_interpolate_pass *pass, ivec2 q0)
{
    *q = (quad_state){.pass = pass, .q0 = q0, .t = pass->t};
    q->scale = (float)(1 << pass->base_level);
    q->inv = v2_inverse_size(pass->width, pass->height);
}

static int shift_down(int value, int bits)
{
    return value >= 0 ? value >> bits : -((-value + (1 << bits) - 1) >> bits);
}

static int candidate_slot(quad_state *q, vec2 u, int mode)
{
    for (int i = 0; i < q->count; i++) {
        if (v2_equal(q->candidates[i].vector, u)) {
            return i;
        }
    }
    int slot = q->count++;
    q->candidates[slot] = (quad_candidate){.vector = u, .mode = mode};
    q->has_uncover = q->has_uncover || mode == MB_MODE_UNCOVER;
    return slot;
}

static vec4 cell_of(const quad_state *q, ivec2 block, ivec2 qm)
{
    const mb_interpolate_pass *pass = q->pass;
    int cx = clampi(shift_down(qm.x - block.x * 8, 2), 0, 1);
    int cy = clampi(shift_down(qm.y - block.y * 8, 2), 0, 1);
    int sx = clampi(block.x * 2 + cx, 0, pass->grid.blocks_x * 2 - 1);
    int sy = clampi(block.y * 2 + cy, 0, pass->grid.blocks_y * 2 - 1);
    return vec4s(pass->cells)[sy * pass->grid.blocks_x * 2 + sx];
}

static void add_block_coverage(quad_state *q, int slot, ivec2 block, int index)
{
    const mb_interpolate_pass *pass = q->pass;
    vec2 center = v2((float)(block.x * 8) + 3.5f, (float)(block.y * 8) + 3.5f);
    vec4 offset = vec4s(pass->offsets)[index];
    for (int j = 0; j < QUAD_PIXELS; j++) {
        vec2 p = at_level(q, pixel_of(q, j));
        float wx = fmaxf(0.0f, 1.0f - fabsf(p.x - center.x) / 8.0f);
        float wy = fmaxf(0.0f, 1.0f - fabsf(p.y - center.y) / 8.0f);
        float w = wx * wy;
        q->candidates[slot].coverage[j] += w;
        q->fallback_sum[j] += w * floats(pass->fallback)[index];
        q->panel_sum[j] += w * floats(pass->panels)[index];
        q->coverage_sum[j] += w;
        q->offset = (vec4){q->offset.x + w * offset.x, q->offset.y + w * offset.y, q->offset.z + w * offset.z,
                           q->offset.w + w};
    }
}

void quad_gather_blocks(quad_state *q)
{
    const mb_interpolate_pass *pass = q->pass;
    ivec2 qm = iv2(q->q0.x >> pass->base_level, q->q0.y >> pass->base_level);
    int qx = qm.x >> 2;
    int qy = qm.y >> 2;
    int bx = (qx & 1) ? qx / 2 : qx / 2 - 1;
    int by = (qy & 1) ? qy / 2 : qy / 2 - 1;
    for (int k = 0; k < 4; k++) {
        ivec2 block = iv2(bx + (k & 1), by + (k >> 1));
        vec4 cell = cell_of(q, block, qm);
        int slot = candidate_slot(q, v2_scale(v4_xy(cell), q->scale), (int)cell.z);
        add_block_coverage(q, slot, block, block_index(block, pass->grid));
    }
    float norm = fmaxf(q->offset.w, 1e-6f);
    q->offset = (vec4){q->offset.x / norm, q->offset.y / norm, q->offset.z / norm, q->offset.w / norm};
    q->luma_offset = q->offset.x * 255.0f * pass->luma_scale;
}

void quad_add_fallback(quad_state *q)
{
    q->fallback_index = q->count++;
    quad_candidate *c = &q->candidates[q->fallback_index];
    *c = (quad_candidate){.vector = v2(0.0f, 0.0f), .mode = MB_MODE_TWO_SIDED};
    for (int j = 0; j < QUAD_PIXELS; j++) {
        c->coverage[j] = 1.0f - q->pass->costs.candidate_prior;
    }
}

float quad_consistent_share(const mb_interpolate_pass *pass, field_probe probe, vec2 u)
{
    float scale = (float)(1 << pass->base_level);
    vec2 p = probe.position;
    block_taps taps = block_taps_at(v2(p.x / scale, p.y / scale), pass->grid);
    vec2 v = v2_scale(u, probe.sign / scale);
    float limit = consistency_limit(pass->costs.consistency, v2_scale(u, 1.0f / scale));
    float share = 0.0f;
    for (int k = 0; k < 4; k++) {
        vec2 d = v2_sub(v4_xy(vec4s(probe.field)[taps.index[k]]), v);
        share += v2_l1(d) <= limit ? taps.weight[k] : 0.0f;
    }
    return share;
}

typedef struct side_trust {
    float a;
    float b;
} side_trust;

static side_trust trust_sides(const quad_state *q, const quad_candidate *c)
{
    if (c->mode != MB_MODE_TWO_SIDED) {
        float a = c->mode == MB_MODE_PAST ? 1.0f : 0.0f;
        float b = c->mode == MB_MODE_FUTURE || c->mode == MB_MODE_UNCOVER ? 1.0f : 0.0f;
        return (side_trust){a, b};
    }
    vec2 center = v2((float)q->q0.x + 0.5f, (float)q->q0.y + 0.5f);
    field_probe forward = {q->pass->forward, 1.0f, v2_add(center, v2_scale(c->vector, -q->t))};
    field_probe backward = {q->pass->backward, -1.0f, v2_add(center, v2_scale(c->vector, 1.0f - q->t))};
    return (side_trust){quad_consistent_share(q->pass, forward, c->vector),
                        quad_consistent_share(q->pass, backward, c->vector)};
}

difference_window quad_difference_window(ivec2 q0, window_sample sample, const void *context)
{
    difference_window w;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            w.d[y * 4 + x] = sample(context, v2((float)(q0.x + x - 1) + 0.5f, (float)(q0.y + y - 1) + 0.5f));
        }
    }
    return w;
}

typedef struct pair_offsets {
    const quad_state *q;
    vec2 a;
    vec2 b;
} pair_offsets;

static float pair_difference(const void *context, vec2 p)
{
    const pair_offsets *o = context;
    const quad_state *q = o->q;
    vec2 sa = v2_add(p, o->a);
    vec2 sb = v2_add(p, o->b);
    float a = ref_sample(&q->pass->a_level->texture, sa.x * q->inv.x, sa.y * q->inv.y, 0);
    float b = ref_sample(&q->pass->b_level->texture, sb.x * q->inv.x, sb.y * q->inv.y, 0);
    return fabsf((a - b) * 255.0f - q->luma_offset);
}

static difference_window differences(const quad_state *q, vec2 u)
{
    pair_offsets offsets = {q, v2_scale(u, -q->t), v2_scale(u, 1.0f - q->t)};
    return quad_difference_window(q->q0, pair_difference, &offsets);
}

float quad_window_error(const difference_window *w, int jx, int jy)
{
    float sum = 0.0f;
    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
            sum += w->d[(jy + dy) * 4 + jx + dx];
        }
    }
    return sum;
}

static float window_minimum(const difference_window *w, int jx, int jy)
{
    float lowest = 1e30f;
    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
            lowest = fminf(lowest, w->d[(jy + dy) * 4 + jx + dx]);
        }
    }
    return lowest;
}

typedef struct side_codes {
    float a;
    float b;
} side_codes;

static side_codes lean_to_nearest(const quad_state *q, side_codes codes, float nearest)
{
    if (q->t < MB_NEAREST_SWITCH) {
        codes.a = fmaxf(codes.a, nearest);
        codes.b = fminf(codes.b, SIDE_LEVELS - codes.a);
    } else {
        codes.b = fmaxf(codes.b, nearest);
        codes.a = fminf(codes.a, SIDE_LEVELS - codes.b);
    }
    return codes;
}

typedef struct candidate_scoring {
    quad_candidate *candidate;
    side_trust trust;
    difference_window window;
} candidate_scoring;

static void score_pixel(const quad_state *q, const candidate_scoring *scoring, int j)
{
    const mb_blend_costs *costs = &q->pass->costs;
    quad_candidate *c = scoring->candidate;
    side_trust s = scoring->trust;
    const difference_window *w = &scoring->window;
    int jx = j & 1;
    int jy = j >> 1;
    float e = quad_window_error(w, jx, jy);
    side_codes codes = {rintf(s.a * (1.0f - s.b) * SIDE_LEVELS), rintf((1.0f - s.a) * s.b * SIDE_LEVELS)};
    float bonus = costs->foreground_bonus * clampf(v2_l1(c->vector) * 0.5f, 0.0f, 1.0f);
    float cost = s.a * s.b * fmaxf(0.0f, e - bonus) + (codes.a + codes.b) / SIDE_LEVELS * costs->one_sided +
                 (1.0f - s.a) * (1.0f - s.b) * (e + costs->unconfirmed);
    if (costs->nearest_side_hi > 0.0f) {
        float nearest = rintf(smoothstepf(costs->nearest_side_lo, costs->nearest_side_hi, window_minimum(w, jx, jy)) *
                              smoothstepf(0.0f, 0.25f, s.a * s.b) * SIDE_LEVELS);
        float before = codes.a + codes.b;
        codes = lean_to_nearest(q, codes, nearest);
        cost += fmaxf(0.0f, codes.a + codes.b - before) / SIDE_LEVELS * costs->one_sided;
    }
    c->cost[j] = cost;
    c->sides[j] = (unsigned)codes.a | ((unsigned)codes.b << SIDE_BITS);
}

void quad_score(quad_state *q)
{
    const mb_blend_costs *costs = &q->pass->costs;
    for (int i = 0; i < q->count; i++) {
        quad_candidate *c = &q->candidates[i];
        if (i == q->fallback_index) {
            for (int j = 0; j < QUAD_PIXELS; j++) {
                c->cost[j] = q->has_uncover ? costs->fallback_uncover : costs->fallback;
                c->sides[j] = 0;
            }
            continue;
        }
        candidate_scoring scoring = {c, trust_sides(q, c), differences(q, c->vector)};
        for (int j = 0; j < QUAD_PIXELS; j++) {
            score_pixel(q, &scoring, j);
        }
    }
}

void quad_soft_scores(const float *cost, int count, float softness, float *score)
{
    float lowest = 1e30f;
    for (int i = 0; i < count; i++) {
        lowest = fminf(lowest, cost[i]);
    }
    for (int i = 0; i < count; i++) {
        float e = expf(-(cost[i] - lowest) / softness);
        score[i] = e < MB_WEIGHT_CUTOFF ? 0.0f : e;
    }
}

static void weigh_pixel(quad_state *q, int j)
{
    const mb_blend_costs *costs = &q->pass->costs;
    float cost[QUAD_MAX_CANDIDATES];
    for (int i = 0; i < q->count; i++) {
        cost[i] = q->candidates[i].cost[j];
    }
    float raw[QUAD_MAX_CANDIDATES];
    quad_soft_scores(cost, q->count, costs->softness, raw);
    float total = 0.0f;
    for (int i = 0; i < q->count; i++) {
        raw[i] *= costs->candidate_prior + q->candidates[i].coverage[j];
        total += raw[i];
    }
    float fallback = q->coverage_sum[j] > 0.0f ? q->fallback_sum[j] / q->coverage_sum[j] : 0.0f;
    for (int i = 0; i < q->count; i++) {
        quad_candidate *c = &q->candidates[i];
        float w = (1.0f - fallback) * raw[i] / total + (i == q->fallback_index ? fallback : 0.0f);
        float a = (float)(c->sides[j] & SIDE_MASK) / SIDE_LEVELS;
        float b = (float)(c->sides[j] >> SIDE_BITS) / SIDE_LEVELS;
        float toward_b = q->t * (1.0f - a - b) + b;
        c->weight_a[j] = w * (1.0f - toward_b);
        c->weight_b[j] = w * toward_b;
    }
}

void quad_weigh(quad_state *q)
{
    for (int j = 0; j < QUAD_PIXELS; j++) {
        weigh_pixel(q, j);
    }
}

static bool contributes(const quad_candidate *c)
{
    float sum = 0.0f;
    for (int j = 0; j < QUAD_PIXELS; j++) {
        sum += c->weight_a[j] + c->weight_b[j];
    }
    return sum > 0.0f;
}

quad_values quad_blend_luma(const quad_state *q)
{
    const mb_interpolate_pass *pass = q->pass;
    quad_values acc = {{0.0f, 0.0f, 0.0f, 0.0f}};
    float dy = q->offset.x;
    for (int i = 0; i < q->count; i++) {
        const quad_candidate *c = &q->candidates[i];
        if (!contributes(c)) {
            continue;
        }
        vec2 base = v2((float)q->q0.x + 0.5f, (float)q->q0.y + 0.5f);
        vec2 pa = v2_sub(base, v2_scale(c->vector, q->t));
        vec2 pb = v2_add(base, v2_scale(c->vector, 1.0f - q->t));
        for (int j = 0; j < QUAD_PIXELS; j++) {
            vec2 step = v2((float)(j & 1), (float)(j >> 1));
            float a = cubic_sample(&pass->frames.a.luma->texture, v2_add(pa, step), 0);
            float b = cubic_sample(&pass->frames.b.luma->texture, v2_add(pb, step), 0);
            acc.v[j] += c->weight_a[j] * (a - q->t * dy) + c->weight_b[j] * (b + (1.0f - q->t) * dy);
        }
    }
    return acc;
}

static float panel_weight(const quad_state *q, int j)
{
    return q->coverage_sum[j] > 0.0f ? q->panel_sum[j] / q->coverage_sum[j] : 0.0f;
}

typedef struct global_offsets {
    vec2 a;
    vec2 b;
} global_offsets;

static global_offsets global_offsets_of(const quad_state *q)
{
    vec2 global = v2_scale(v4_xy(vec4s(q->pass->global_motion)[0]), q->scale);
    return (global_offsets){v2_scale(global, -q->t), v2_scale(global, 1.0f - q->t)};
}

static float sample_global_chroma(quad_state *q, global_offsets g)
{
    const mb_interpolate_pass *pass = q->pass;
    const mb_chroma_layout *chroma = &pass->chroma;
    vec2 qc = chroma_from_luma(chroma, v2((float)q->q0.x + 1.0f, (float)q->q0.y + 1.0f));
    q->global_chroma_a = chroma_sample(&pass->frames.a, chroma, v2_add(qc, chroma_from_luma(chroma, g.a)));
    q->global_chroma_b = chroma_sample(&pass->frames.b, chroma, v2_add(qc, chroma_from_luma(chroma, g.b)));
    vec2 a = q->global_chroma_a;
    vec2 b = q->global_chroma_b;
    return fmaxf(fabsf(a.x - b.x), fabsf(a.y - b.y));
}

static float sample_global_luma(quad_state *q, global_offsets g)
{
    const mb_interpolate_pass *pass = q->pass;
    float change = 0.0f;
    for (int j = 0; j < QUAD_PIXELS; j++) {
        ivec2 p = pixel_of(q, j);
        vec2 pp = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
        vec2 sa = v2_add(pp, g.a);
        vec2 sb = v2_add(pp, g.b);
        q->global_a[j] = ref_sample(&pass->frames.a.luma->texture, sa.x * q->inv.x, sa.y * q->inv.y, 0);
        q->global_b[j] = ref_sample(&pass->frames.b.luma->texture, sb.x * q->inv.x, sb.y * q->inv.y, 0);
        change = fmaxf(change, fabsf(q->global_a[j] - q->global_b[j]));
    }
    return change;
}

void quad_measure_panels(quad_state *q)
{
    const mb_interpolate_pass *pass = q->pass;
    q->panel_share = 0.0f;
    for (int j = 0; j < QUAD_PIXELS; j++) {
        q->panel_share += panel_weight(q, j) * 0.25f;
    }
    if (q->panel_share <= 0.0f) {
        return;
    }
    global_offsets g = global_offsets_of(q);
    float chroma_change = sample_global_chroma(q, g);
    float luma_change = sample_global_luma(q, g);
    float change = fmaxf(chroma_change, luma_change) * 255.0f * pass->luma_scale;
    q->panel_switch = smoothstepf(pass->costs.panel_switch, pass->costs.panel_switch * 2.5f, change);
}

static float unmoved_value(const quad_state *q, float a, float b)
{
    float blend = (1.0f - q->t) * a + q->t * b;
    return mixf(blend, q->t < MB_NEAREST_SWITCH ? a : b, q->panel_switch);
}

quad_values quad_mix_panels_luma(const quad_state *q, quad_values luma)
{
    for (int j = 0; j < QUAD_PIXELS; j++) {
        float share = panel_weight(q, j);
        if (share > 0.0f) {
            luma.v[j] = mixf(luma.v[j], unmoved_value(q, q->global_a[j], q->global_b[j]), share);
        }
    }
    return luma;
}

typedef struct chroma_weights {
    float a;
    float b;
} chroma_weights;

void quad_visit_chroma(const mb_chroma_layout *chroma, ivec2 q0, chroma_visit visit, const void *context)
{
    for (int cy = 0; cy < (2 >> chroma->shift_y); cy++) {
        for (int cx = 0; cx < (2 >> chroma->shift_x); cx++) {
            ivec2 cp = iv2((q0.x >> chroma->shift_x) + cx, (q0.y >> chroma->shift_y) + cy);
            if (cp.x < chroma->width && cp.y < chroma->height) {
                visit(context, iv2(cx, cy), cp);
            }
        }
    }
}

covered_pixels quad_covered_pixels(const mb_chroma_layout *chroma, ivec2 chroma_index)
{
    covered_pixels cover = {.count = 0};
    for (int yy = 0; yy < (1 << chroma->shift_y); yy++) {
        for (int xx = 0; xx < (1 << chroma->shift_x); xx++) {
            cover.index[cover.count++] =
                ((chroma_index.y << chroma->shift_y) + yy) * 2 + (chroma_index.x << chroma->shift_x) + xx;
        }
    }
    return cover;
}

static chroma_weights covered_weights(const quad_state *q, const quad_candidate *c, ivec2 index)
{
    covered_pixels cover = quad_covered_pixels(&q->pass->chroma, index);
    chroma_weights w = {0.0f, 0.0f};
    for (int k = 0; k < cover.count; k++) {
        w.a += c->weight_a[cover.index[k]];
        w.b += c->weight_b[cover.index[k]];
    }
    float norm = 1.0f / (float)cover.count;
    return (chroma_weights){w.a * norm, w.b * norm};
}

static vec2 global_chroma(const quad_state *q, vec2 pc, bool first)
{
    const mb_interpolate_pass *pass = q->pass;
    if (pass->chroma.shift_x == 1 && pass->chroma.shift_y == 1) {
        return first ? q->global_chroma_a : q->global_chroma_b;
    }
    vec2 global = v2_scale(v4_xy(vec4s(pass->global_motion)[0]), q->scale);
    vec2 g = v2_scale(global, first ? -q->t : 1.0f - q->t);
    vec2 p = v2_add(pc, chroma_from_luma(&pass->chroma, g));
    const mb_frame_planes *planes = first ? &pass->frames.a : &pass->frames.b;
    return chroma_sample(planes, &pass->chroma, p);
}

vec2 quad_blend_chroma(const quad_state *q, ivec2 chroma_index, ivec2 chroma_position_index)
{
    const mb_interpolate_pass *pass = q->pass;
    vec2 pc = v2((float)chroma_position_index.x + 0.5f, (float)chroma_position_index.y + 0.5f);
    vec2 dcc = v2(q->offset.y, q->offset.z);
    vec2 acc = v2(0.0f, 0.0f);
    for (int i = 0; i < q->count; i++) {
        const quad_candidate *c = &q->candidates[i];
        chroma_weights w = covered_weights(q, c, chroma_index);
        if (w.a + w.b <= 0.0f) {
            continue;
        }
        vec2 pa = v2_sub(pc, chroma_from_luma(&pass->chroma, v2_scale(c->vector, q->t)));
        vec2 pb = v2_add(pc, chroma_from_luma(&pass->chroma, v2_scale(c->vector, 1.0f - q->t)));
        vec2 a = chroma_cubic_sample(&pass->frames.a, &pass->chroma, pa);
        vec2 b = chroma_cubic_sample(&pass->frames.b, &pass->chroma, pb);
        acc = v2_add(acc, v2_add(v2_scale(v2_sub(a, v2_scale(dcc, q->t)), w.a),
                                 v2_scale(v2_add(b, v2_scale(dcc, 1.0f - q->t)), w.b)));
    }
    if (q->panel_share > 0.0f) {
        vec2 sa = global_chroma(q, pc, true);
        vec2 sb = global_chroma(q, pc, false);
        vec2 still = v2(unmoved_value(q, sa.x, sb.x), unmoved_value(q, sa.y, sb.y));
        acc = v2(mixf(acc.x, still.x, q->panel_share), mixf(acc.y, still.y, q->panel_share));
    }
    return acc;
}

float quad_output_value(const mb_output_range *range, float value)
{
    if (!range->sixteen_bit) {
        return value;
    }
    float v = fminf(value, range->max_value);
    return range->step > 0.0f ? rintf(v / range->step) * range->step : v;
}

void quad_store_luma(const mb_interpolate_pass *pass, ivec2 p, float value)
{
    if (p.x < pass->width && p.y < pass->height) {
        ref_store(&pass->output.luma->texture, p.x, p.y, 0, quad_output_value(&pass->range, value));
    }
}

void quad_store_chroma_raw(const mb_interpolate_pass *pass, ivec2 p, vec2 value)
{
    chroma_channels out = chroma_channels_of(&pass->output, &pass->chroma);
    ref_store(out.first, p.x, p.y, 0, value.x);
    ref_store(out.second, p.x, p.y, out.second_channel, value.y);
}

void quad_store_chroma(const mb_interpolate_pass *pass, ivec2 p, vec2 value)
{
    quad_store_chroma_raw(pass, p,
                          v2(quad_output_value(&pass->range, value.x), quad_output_value(&pass->range, value.y)));
}
