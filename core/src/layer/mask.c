#include "layer/common.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { RADIUS = MB_LAYER_EVIDENCE_RADIUS, TAPS = (2 * RADIUS + 1) * (2 * RADIUS + 1) };

typedef struct side_evidence {
    float still;
    float moving;
    float prior;
    float known;
    bool has_motion;
} side_evidence;

static float still_error(const ref_texture *frame, const ref_texture *other, int x, int y)
{
    float sum = 0.0f;
    for (int dy = -RADIUS; dy <= RADIUS; dy++) {
        for (int dx = -RADIUS; dx <= RADIUS; dx++) {
            sum += fabsf(ref_texel(frame, x + dx, y + dy, 0) - ref_texel(other, x + dx, y + dy, 0));
        }
    }
    return sum / (float)TAPS * 255.0f;
}

static bool moving_error(const ref_texture *frame, const ref_texture *other, int x, int y, vec2 v, float *error)
{
    float sum = 0.0f;
    float count = 0.0f;
    for (int dy = -RADIUS; dy <= RADIUS; dy++) {
        for (int dx = -RADIUS; dx <= RADIUS; dx++) {
            int rx = clampi(x + dx, 0, frame->width - 1);
            int ry = clampi(y + dy, 0, frame->height - 1);
            float sx = (float)rx + v.x;
            float sy = (float)ry + v.y;
            if (sx >= 0.0f && sx <= (float)(other->width - 1) && sy >= 0.0f && sy <= (float)(other->height - 1)) {
                float moved = ref_sample(other, (sx + 0.5f) / (float)other->width, (sy + 0.5f) / (float)other->height, 0);
                sum += fabsf(ref_texel(frame, rx, ry, 0) - moved);
                count += 1.0f;
            }
        }
    }
    *error = sum / fmaxf(count, 1.0f) * 255.0f;
    return count > 0.5f * (float)TAPS;
}

static side_evidence side_at(const mb_layer_evidence_pass *pass, const mb_layer_side *side, int x, int y)
{
    const ref_texture *frame = &pass->frame->texture;
    const ref_texture *other = &side->frame->texture;
    side_evidence e = {still_error(frame, other, x, y), 0.0f, 0.0f, 0.0f, false};
    if (layer_is_active(side->model)) {
        vec2 top = v2(((float)x + 0.5f) * 0.5f, ((float)y + 0.5f) * 0.5f);
        vec4 f = layer_field_at(vec4s(side->field), pass->grid, top);
        e.has_motion = moving_error(frame, other, x, y, v2_scale(v4_xy(f), 2.0f), &e.moving);
        e.prior = f.z + (1.0f - f.w);
        e.known = f.w;
    }
    return e;
}

static float softness_at(const mb_layer_evidence_pass *pass, int x, int y)
{
    const ref_texture *detail = &pass->detail->texture;
    int step = pass->detail_step;
    float sum = 0.0f;
    for (int j = 0; j < 3 * step; j++) {
        for (int i = 0; i < 3 * step; i++) {
            int px = clampi((x - 1) * step + i, 0, detail->width - 1);
            int py = clampi((y - 1) * step + j, 0, detail->height - 1);
            float here = ref_texel(detail, px, py, 0);
            sum += fabsf(ref_texel(detail, px + 1, py, 0) - here) + fabsf(ref_texel(detail, px, py + 1, 0) - here);
        }
    }
    float level = sum / (float)(9 * step * step) * 255.0f;
    return 1.0f - smoothstepf((float)MB_LAYER_DETAIL_LO, (float)MB_LAYER_DETAIL_HI, level);
}

static vec2 evidence_at(const mb_layer_evidence_pass *pass, int x, int y)
{
    float still = 1e30f;
    float moving = 0.0f;
    float prior = 2.0f;
    float known = 0.0f;
    bool any = false;
    for (int s = 0; s < 2; s++) {
        const mb_layer_side *side = &pass->sides[s];
        if (!side->frame || layer_is_cut(side->stats)) {
            continue;
        }
        side_evidence e = side_at(pass, side, x, y);
        still = fminf(still, e.still);
        if (e.has_motion) {
            moving = fmaxf(moving, e.moving);
            prior = fminf(prior, e.prior);
            known = fmaxf(known, e.known);
            any = true;
        }
    }
    if (!any) {
        return v2(0.0f, 0.0f);
    }
    float clip = (float)MB_LAYER_EVIDENCE_CLIP;
    float trusted = fminf(prior, 1.0f) * clampf(2.0f * known, 0.0f, 1.0f);
    return v2(clampf(still - moving, -clip, clip), trusted * softness_at(pass, x, y));
}

void ref_layer_evidence(const mb_layer_evidence_pass *pass)
{
    mb_buffer *models[2] = {pass->sides[0].model, pass->sides[1].model};
    if (!layer_gate_open(models, pass->trigger)) {
        return;
    }
    vec2 *evidence = vec2s(pass->evidence);
    for (int y = 0; y < pass->height; y++) {
        for (int x = 0; x < pass->width; x++) {
            evidence[y * pass->width + x] = evidence_at(pass, x, y);
        }
    }
}

static float blurred(const mb_layer_blur_pass *pass, const layer_kernel *k, int x, int y)
{
    const vec2 *source = vec2s(pass->source);
    float sum = 0.0f;
    for (int dy = -k->radius; dy <= k->radius; dy++) {
        for (int dx = -k->radius; dx <= k->radius; dx++) {
            int rx = clampi(x + dx, 0, pass->width - 1);
            int ry = clampi(y + dy, 0, pass->height - 1);
            sum += source[ry * pass->width + rx].x * k->weights[dy + k->radius] * k->weights[dx + k->radius];
        }
    }
    return sum;
}

static float decided(float evidence, float prior)
{
    if (prior <= (float)MB_LAYER_PRIOR_FLOOR) {
        return 0.0f;
    }
    float score = evidence + (prior - 0.5f) * (float)MB_LAYER_PRIOR_BIAS;
    return clampf(score / (float)MB_LAYER_DECIDE_SOFTNESS + 0.5f, 0.0f, 1.0f);
}

static float nearness(const mb_layer_blur_pass *pass, const layer_kernel *k, int x, int y)
{
    const vec2 *source = vec2s(pass->source);
    float sum = 0.0f;
    for (int j = -k->radius; j <= k->radius; j++) {
        for (int i = -k->radius; i <= k->radius; i++) {
            int rx = clampi(x + i * MB_LAYER_NEAR_STEP, 0, pass->width - 1);
            int ry = clampi(y + j * MB_LAYER_NEAR_STEP, 0, pass->height - 1);
            sum += source[ry * pass->width + rx].x * k->weights[j + k->radius] * k->weights[i + k->radius];
        }
    }
    return clampf((float)MB_LAYER_NEAR_GAIN * (sum - (float)MB_LAYER_NEAR_FLOOR), 0.0f, 1.0f);
}

void ref_layer_blur(const mb_layer_blur_pass *pass)
{
    vec2 *target = vec2s(pass->target);
    if (!layer_gate_open(pass->gate.models, pass->gate.trigger)) {
        if (!pass->decide && !layer_any_active(pass->gate.models)) {
            for (int i = 0; i < pass->width * pass->height; i++) {
                target[i] = v2(0.0f, 0.0f);
            }
        }
        return;
    }
    layer_kernel near = layer_gauss((float)MB_LAYER_NEAR_SIGMA / (float)MB_LAYER_NEAR_STEP, MB_LAYER_NEAR_TAPS);
    layer_kernel k = pass->decide ? layer_gauss((float)MB_LAYER_DECIDE_SIGMA, MB_LAYER_DECIDE_RADIUS)
                                  : layer_gauss((float)MB_LAYER_SOFTEN_SIGMA, MB_LAYER_SOFTEN_RADIUS);
    const vec2 *source = vec2s(pass->source);
    for (int y = 0; y < pass->height; y++) {
        for (int x = 0; x < pass->width; x++) {
            float value = blurred(pass, &k, x, y);
            float prior = source[y * pass->width + x].y;
            target[y * pass->width + x] = pass->decide ? v2(decided(value, prior), 0.0f) : v2(value, nearness(pass, &near, x, y));
        }
    }
}

static float guard_at(const mb_layer_guard_pass *pass, int x, int y)
{
    const ref_texture *a = &pass->a->texture;
    const ref_texture *b = &pass->b->texture;
    const ref_texture *out = &pass->output->texture;
    float sum = 0.0f;
    for (int j = 0; j < MB_LAYER_GUARD_TAPS; j++) {
        for (int i = 0; i < MB_LAYER_GUARD_TAPS; i++) {
            int px = clampi(x * pass->step + (2 * i + 1) * pass->step / 4 - pass->step / 2, 0, pass->frame_width - 1);
            int py = clampi(y * pass->step + (2 * j + 1) * pass->step / 4 - pass->step / 2, 0, pass->frame_height - 1);
            float still = mixf(ref_texel(a, px, py, 0), ref_texel(b, px, py, 0), pass->t);
            sum += fabsf(ref_texel(out, px, py, 0) - still);
        }
    }
    return sum / (float)(MB_LAYER_GUARD_TAPS * MB_LAYER_GUARD_TAPS) * 255.0f * pass->luma_scale;
}

static bool near_layer(const mb_layer_guard_pass *pass, int x, int y)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int i = clampi(y + dy, 0, pass->height - 1) * pass->width + clampi(x + dx, 0, pass->width - 1);
            if (vec2s(pass->masks[0])[i].y > 0.0f || vec2s(pass->masks[1])[i].y > 0.0f) {
                return true;
            }
        }
    }
    return false;
}

void ref_layer_guard(const mb_layer_guard_pass *pass)
{
    vec2 *guard = vec2s(pass->guard);
    bool active = layer_any_active(pass->models) && !layer_is_cut(pass->stats);
    for (int y = 0; y < pass->height; y++) {
        for (int x = 0; x < pass->width; x++) {
            bool used = active && near_layer(pass, x, y);
            guard[y * pass->width + x] = v2(used ? guard_at(pass, x, y) : 0.0f, 0.0f);
        }
    }
}
