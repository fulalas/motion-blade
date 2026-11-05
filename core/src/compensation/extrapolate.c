#include "compensation/extrapolate.h"

#include "common/blocks.h"
#include "compensation/chroma.h"
#include "compensation/cubic.h"
#include "compensation/quad.h"
#include "motionblade/layout.h"
#include "reference/cpu_resources.h"

typedef struct extrapolation {
    const mb_interpolate_pass *pass;
    const mb_extrapolation_side *side;
    const mb_frame_planes *source;
    const mb_image *source_level;
    float reach;
    float scale;
    vec2 inv;
} extrapolation;

typedef struct extrapolated_quad {
    ivec2 q0;
    vec2 center;
    vec2 vectors[MB_EXTRAPOLATION_VECTORS];
    int count;
    float costs[MB_EXTRAPOLATION_VECTORS][QUAD_PIXELS];
    float weights[MB_EXTRAPOLATION_VECTORS][QUAD_PIXELS];
} extrapolated_quad;

static bool usable(const mb_extrapolation_side *side)
{
    return side->field && words(side->stats)[MB_STAT_CUT] == 0;
}

cut_plan cut_plan_of(const mb_interpolate_pass *pass)
{
    bool past = usable(&pass->extrapolation.past);
    bool next = usable(&pass->extrapolation.next);
    bool near_a = pass->t < MB_CUT_SIDE_SWITCH;
    if (!past && !next) {
        return (cut_plan){false, !near_a};
    }
    return (cut_plan){true, next && !(past && near_a)};
}

static extrapolation extrapolation_of(const mb_interpolate_pass *pass, bool from_b)
{
    return (extrapolation){
        .pass = pass,
        .side = from_b ? &pass->extrapolation.next : &pass->extrapolation.past,
        .source = from_b ? &pass->frames.b : &pass->frames.a,
        .source_level = from_b ? pass->b_level : pass->a_level,
        .reach = from_b ? 1.0f - pass->t : pass->t,
        .scale = (float)(1 << pass->base_level),
        .inv = v2_inverse_size(pass->width, pass->height),
    };
}

static vec2 field_at(const extrapolation *ex, int index)
{
    return v4_xy(vec4s(ex->side->field)[index]);
}

static vec2 field_vector(const extrapolation *ex, vec2 p)
{
    block_taps taps = block_taps_at(v2(p.x / ex->scale, p.y / ex->scale), ex->pass->grid);
    vec2 sum = v2(0.0f, 0.0f);
    for (int k = 0; k < 4; k++) {
        sum = v2_add(sum, v2_scale(field_at(ex, taps.index[k]), taps.weight[k]));
    }
    return v2_scale(sum, ex->scale);
}

static void add_vector(extrapolated_quad *quad, vec2 w)
{
    for (int i = 0; i < quad->count; i++) {
        if (v2_l1(v2_sub(quad->vectors[i], w)) < MB_SAME_VECTOR) {
            return;
        }
    }
    quad->vectors[quad->count++] = w;
}

static void gather_vectors(const extrapolation *ex, extrapolated_quad *quad)
{
    vec2 first = v2_scale(field_vector(ex, quad->center), ex->reach);
    add_vector(quad, field_vector(ex, v2_add(quad->center, first)));
    ivec2 block = block_at(v2_scale(quad->center, 1.0f / ex->scale));
    for (int k = 0; k < 9; k++) {
        add_vector(quad, v2_scale(field_at(ex, neighbour3_index(k, block, ex->pass->grid)), ex->scale));
    }
}

static float level_sample(const extrapolation *ex, const mb_image *level, vec2 p)
{
    return ref_sample(&level->texture, p.x * ex->inv.x, p.y * ex->inv.y, 0);
}

typedef struct extrapolated_vector {
    const extrapolation *ex;
    vec2 w;
} extrapolated_vector;

static float neighbour_difference(const void *context, vec2 p)
{
    const extrapolated_vector *v = context;
    const extrapolation *ex = v->ex;
    float s = level_sample(ex, ex->source_level, v2_add(p, v2_scale(v->w, ex->reach)));
    float o = level_sample(ex, ex->side->neighbour_level, v2_add(p, v2_scale(v->w, 1.0f + ex->reach)));
    return fabsf(s - o) * 255.0f;
}

static difference_window differences(const extrapolation *ex, ivec2 q0, vec2 w)
{
    extrapolated_vector vector = {ex, w};
    return quad_difference_window(q0, neighbour_difference, &vector);
}

static float unsettled_cost(const extrapolation *ex, const extrapolated_quad *quad, vec2 w)
{
    field_probe probe = {ex->side->field, 1.0f, v2_add(quad->center, v2_scale(w, ex->reach))};
    return (1.0f - quad_consistent_share(ex->pass, probe, w)) * ex->pass->costs.unconfirmed;
}

static void score_vectors(const extrapolation *ex, extrapolated_quad *quad)
{
    for (int i = 0; i < quad->count; i++) {
        difference_window window = differences(ex, quad->q0, quad->vectors[i]);
        float unsettled = unsettled_cost(ex, quad, quad->vectors[i]);
        for (int j = 0; j < QUAD_PIXELS; j++) {
            quad->costs[i][j] = quad_window_error(&window, j & 1, j >> 1) + unsettled;
        }
    }
}

static void weigh_pixel(const extrapolation *ex, extrapolated_quad *quad, int j)
{
    float cost[MB_EXTRAPOLATION_VECTORS];
    float score[MB_EXTRAPOLATION_VECTORS];
    for (int i = 0; i < quad->count; i++) {
        cost[i] = quad->costs[i][j];
    }
    quad_soft_scores(cost, quad->count, ex->pass->costs.softness, score);
    float total = 0.0f;
    for (int i = 0; i < quad->count; i++) {
        total += score[i];
    }
    for (int i = 0; i < quad->count; i++) {
        quad->weights[i][j] = score[i] / total;
    }
}

static void extrapolate_luma(const extrapolation *ex, const extrapolated_quad *quad)
{
    for (int j = 0; j < QUAD_PIXELS; j++) {
        ivec2 p = iv2(quad->q0.x + (j & 1), quad->q0.y + (j >> 1));
        vec2 pc = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
        float sum = 0.0f;
        for (int i = 0; i < quad->count; i++) {
            if (quad->weights[i][j] > 0.0f) {
                vec2 at = v2_add(pc, v2_scale(quad->vectors[i], ex->reach));
                sum += quad->weights[i][j] * cubic_sample(&ex->source->luma->texture, at, 0);
            }
        }
        quad_store_luma(ex->pass, p, sum);
    }
}

static float covered_weight(const extrapolation *ex, const extrapolated_quad *quad, int i, ivec2 index)
{
    covered_pixels cover = quad_covered_pixels(&ex->pass->chroma, index);
    float sum = 0.0f;
    for (int k = 0; k < cover.count; k++) {
        sum += quad->weights[i][cover.index[k]];
    }
    return sum / (float)cover.count;
}

static vec2 extrapolated_chroma(const extrapolation *ex, const extrapolated_quad *quad, ivec2 index, ivec2 cp)
{
    const mb_chroma_layout *chroma = &ex->pass->chroma;
    vec2 pc = v2((float)cp.x + 0.5f, (float)cp.y + 0.5f);
    vec2 acc = v2(0.0f, 0.0f);
    for (int i = 0; i < quad->count; i++) {
        float w = covered_weight(ex, quad, i, index);
        if (w > 0.0f) {
            vec2 at = v2_add(pc, chroma_from_luma(chroma, v2_scale(quad->vectors[i], ex->reach)));
            acc = v2_add(acc, v2_scale(chroma_cubic_sample(ex->source, chroma, at), w));
        }
    }
    return acc;
}

typedef struct quad_extrapolation {
    const extrapolation *ex;
    const extrapolated_quad *quad;
} quad_extrapolation;

static void store_extrapolated_chroma(const void *context, ivec2 index, ivec2 cp)
{
    const quad_extrapolation *e = context;
    quad_store_chroma(e->ex->pass, cp, extrapolated_chroma(e->ex, e->quad, index, cp));
}

static void extrapolate_chroma(const extrapolation *ex, const extrapolated_quad *quad)
{
    quad_extrapolation e = {ex, quad};
    quad_visit_chroma(&ex->pass->chroma, quad->q0, store_extrapolated_chroma, &e);
}

static void extrapolate_quad(const extrapolation *ex, ivec2 q0)
{
    extrapolated_quad quad = {.q0 = q0, .center = v2((float)q0.x + 1.0f, (float)q0.y + 1.0f)};
    gather_vectors(ex, &quad);
    score_vectors(ex, &quad);
    for (int j = 0; j < QUAD_PIXELS; j++) {
        weigh_pixel(ex, &quad, j);
    }
    extrapolate_luma(ex, &quad);
    extrapolate_chroma(ex, &quad);
}

void ref_extrapolate(const mb_interpolate_pass *pass, bool from_b)
{
    extrapolation ex = extrapolation_of(pass, from_b);
    for (int y = 0; y < pass->height; y += 2) {
        for (int x = 0; x < pass->width; x += 2) {
            extrapolate_quad(&ex, iv2(x, y));
        }
    }
}
