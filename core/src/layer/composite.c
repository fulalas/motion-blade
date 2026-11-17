#include "compensation/chroma.h"
#include "compensation/quad.h"
#include "layer/common.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

typedef struct layer_quad {
    vec2 motion;
    float alpha;
    float weight_a;
    float weight_b;
    float both;
    float only_a;
    float only_b;
    float hidden;
    float distrust;
} layer_quad;

static vec4 direction_field(const mb_layer_composite_pass *pass, int direction, vec2 position)
{
    float scale = (float)pass->top_scale;
    bool own = layer_is_active(pass->models[direction]);
    const vec4 *field = vec4s(pass->fields[own ? direction : 1 - direction]);
    vec4 f = layer_field_at(field, pass->grid, v2_scale(position, 1.0f / scale));
    float sign = own ? scale : -scale;
    return (vec4){f.x * sign, f.y * sign, f.z, 0.0f};
}

static float edge_weight(const mb_layer_composite_pass *pass, vec2 p)
{
    float w = (float)pass->warp.width;
    float h = (float)pass->warp.height;
    float d = fminf(fminf(p.x - 0.5f, w - 0.5f - p.x), fminf(p.y - 0.5f, h - 0.5f - p.y));
    if (d < 0.0f) {
        return 0.0f;
    }
    float ramp = (float)MB_LAYER_EDGE_RAMP * (float)pass->top_scale;
    return clampf(d / ramp + (float)MB_LAYER_EDGE_FLOOR, 0.0f, 1.0f);
}

static vec2 mask_at(const mb_layer_composite_pass *pass, const mb_buffer *mask, vec2 p)
{
    float scale = (float)pass->mask_scale;
    vec2 index = v2(p.x / scale - 0.5f, p.y / scale - 0.5f);
    return layer_mask_at(vec2s(mask), pass->mask_width, pass->mask_height, index);
}

static layer_quad quad_at(const mb_layer_composite_pass *pass, vec2 c)
{
    float t = pass->warp.t;
    vec4 a0 = direction_field(pass, 0, c);
    vec4 b0 = direction_field(pass, 1, c);
    vec4 a1 = direction_field(pass, 0, v2_sub(c, v2_scale(v4_xy(a0), t)));
    vec4 b1 = direction_field(pass, 1, v2_sub(c, v2_scale(v4_xy(b0), 1.0f - t)));
    layer_quad q;
    q.motion = v2_sub(v2_scale(v4_xy(a1), 1.0f - t), v2_scale(v4_xy(b1), t));
    vec2 from_a = v2_sub(c, v2_scale(q.motion, t));
    vec2 from_b = v2_add(c, v2_scale(q.motion, 1.0f - t));
    float va = edge_weight(pass, from_a);
    float vb = edge_weight(pass, from_b);
    float ma = mask_at(pass, pass->masks[0], from_a).x;
    float mb = mask_at(pass, pass->masks[1], from_b).x;
    float sa = clampf(2.0f * va, 0.0f, 1.0f);
    float sb = clampf(2.0f * vb, 0.0f, 1.0f);
    q.alpha = fminf(ma, mb) * sa * sb + ma * sa * (1.0f - sb) + mb * sb * (1.0f - sa);
    q.weight_a = (1.0f - t) * va;
    q.weight_b = t * vb;
    vec2 here_a = mask_at(pass, pass->masks[0], c);
    vec2 here_b = mask_at(pass, pass->masks[1], c);
    q.both = (1.0f - here_a.x) * (1.0f - here_b.x);
    q.only_a = (1.0f - here_a.x) * here_b.x;
    q.only_b = here_a.x * (1.0f - here_b.x);
    q.hidden = here_a.x * here_b.x;
    float near = fmaxf(here_a.y, here_b.y);
    float excess = (mask_at(pass, pass->guard, c).x - (float)MB_LAYER_GUARD_LO) / (float)MB_LAYER_GUARD_SPAN;
    q.distrust = clampf(excess, 0.0f, 1.0f) * near;
    return q;
}

static bool untouched(const layer_quad *q)
{
    float skip = (float)MB_LAYER_SKIP;
    return q->alpha < skip && q->both > 1.0f - skip && q->distrust < skip;
}

static float composed(const layer_quad *q, float t, float moved_a, float moved_b, float a, float b, float current)
{
    float front = (moved_a * q->weight_a + moved_b * q->weight_b) / fmaxf(q->weight_a + q->weight_b, 1e-6f);
    float still = mixf(a, b, t);
    float back = mixf(current, still, q->distrust) * q->both + a * q->only_a + b * q->only_b + still * q->hidden;
    return front * q->alpha + back * (1.0f - q->alpha);
}

static void compose_luma(const mb_layer_composite_pass *pass, const layer_quad *q, ivec2 p)
{
    const mb_warp *w = &pass->warp;
    if (p.x >= w->width || p.y >= w->height) {
        return;
    }
    const ref_texture *a = &w->frames.a.luma->texture;
    const ref_texture *b = &w->frames.b.luma->texture;
    vec2 c = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
    vec2 from_a = v2_sub(c, v2_scale(q->motion, w->t));
    vec2 from_b = v2_add(c, v2_scale(q->motion, 1.0f - w->t));
    float moved_a = ref_sample(a, from_a.x / (float)w->width, from_a.y / (float)w->height, 0);
    float moved_b = ref_sample(b, from_b.x / (float)w->width, from_b.y / (float)w->height, 0);
    float current = ref_texel(&w->output.luma->texture, p.x, p.y, 0);
    float value = composed(q, w->t, moved_a, moved_b, ref_texel(a, p.x, p.y, 0), ref_texel(b, p.x, p.y, 0), current);
    quad_store_luma(w, p, value);
}

static void compose_chroma(const mb_layer_composite_pass *pass, const layer_quad *q, ivec2 p)
{
    const mb_warp *w = &pass->warp;
    const mb_chroma_layout *chroma = &w->chroma;
    if (p.x >= chroma->width || p.y >= chroma->height) {
        return;
    }
    vec2 c = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
    vec2 motion = chroma_from_luma(chroma, q->motion);
    vec2 moved_a = chroma_sample(&w->frames.a, chroma, v2_sub(c, v2_scale(motion, w->t)));
    vec2 moved_b = chroma_sample(&w->frames.b, chroma, v2_add(c, v2_scale(motion, 1.0f - w->t)));
    vec2 a = chroma_texel(&w->frames.a, chroma, p);
    vec2 b = chroma_texel(&w->frames.b, chroma, p);
    vec2 current = chroma_texel(&w->output, chroma, p);
    vec2 value = v2(composed(q, w->t, moved_a.x, moved_b.x, a.x, b.x, current.x),
                    composed(q, w->t, moved_a.y, moved_b.y, a.y, b.y, current.y));
    quad_store_chroma(w, p, value);
}

static void compose_quad(const mb_layer_composite_pass *pass, ivec2 q0)
{
    const mb_chroma_layout *chroma = &pass->warp.chroma;
    layer_quad q = quad_at(pass, v2((float)q0.x + 1.0f, (float)q0.y + 1.0f));
    if (untouched(&q)) {
        return;
    }
    for (int j = 0; j < 4; j++) {
        compose_luma(pass, &q, iv2(q0.x + (j & 1), q0.y + (j >> 1)));
    }
    for (int cy = 0; cy < (2 >> chroma->shift_y); cy++) {
        for (int cx = 0; cx < (2 >> chroma->shift_x); cx++) {
            compose_chroma(pass, &q, iv2((q0.x >> chroma->shift_x) + cx, (q0.y >> chroma->shift_y) + cy));
        }
    }
}

void ref_layer_composite(const mb_layer_composite_pass *pass)
{
    const mb_warp *w = &pass->warp;
    if (!layer_any_active(pass->models) || layer_is_cut(w->stats)) {
        return;
    }
    for (int y = 0; y < w->height; y += 2) {
        for (int x = 0; x < w->width; x += 2) {
            compose_quad(pass, iv2(x, y));
        }
    }
}
