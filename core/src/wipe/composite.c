#include "compensation/chroma.h"
#include "compensation/quad.h"
#include "reference/kernels.h"
#include "wipe/common.h"

typedef struct wipe_switch {
    float weight;
    bool swept;
} wipe_switch;

static wipe_switch switch_at(const mb_wipe_composite_pass *pass, vec2 position)
{
    vec2 w = v2_scale(position, 1.0f / (float)pass->texel_scale);
    int cx = clampi((int)floorf(w.x / (float)MB_WIPE_CELL), 0, pass->grid.cells_x - 1);
    int cy = clampi((int)floorf(w.y / (float)MB_WIPE_CELL), 0, pass->grid.cells_y - 1);
    const vec4 *fronts = vec4s(pass->fronts) + MB_WIPE_FRONT_VALUES * (cy * pass->grid.cells_x + cx);
    if (fronts[1].x < 0.5f) {
        return (wipe_switch){0.0f, false};
    }
    bool rows = fronts[1].x < 1.5f;
    float along = rows ? w.x : w.y;
    float lateral = rows ? w.y : w.x;
    float from = fronts[0].x + fronts[0].y * lateral;
    float span = fronts[0].z + fronts[0].w * lateral - from;
    if (fabsf(span) < 1.0f) {
        return (wipe_switch){0.0f, false};
    }
    float passed = (along - from) / span;
    float weight = clampf((pass->warp.t - passed) * fabsf(span) * (float)pass->texel_scale + 0.5f, 0.0f, 1.0f);
    return (wipe_switch){weight, passed >= 0.0f && passed <= 1.0f};
}

static void compose_luma(const mb_wipe_composite_pass *pass, ivec2 p)
{
    const mb_warp *w = &pass->warp;
    vec2 c = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
    wipe_switch s = switch_at(pass, c);
    if (!s.swept) {
        return;
    }
    float old = ref_texel(&w->frames.a.luma->texture, p.x, p.y, 0);
    quad_store_luma(w, p, mixf(old, ref_texel(&w->frames.b.luma->texture, p.x, p.y, 0), s.weight));
}

static void compose_chroma(const mb_wipe_composite_pass *pass, ivec2 p)
{
    const mb_warp *w = &pass->warp;
    const mb_chroma_layout *chroma = &w->chroma;
    vec2 c = v2((float)p.x + 0.5f, (float)p.y + 0.5f);
    vec2 luma_position = v2(c.x * (float)(1 << chroma->shift_x), c.y * (float)(1 << chroma->shift_y));
    wipe_switch s = switch_at(pass, luma_position);
    if (!s.swept) {
        return;
    }
    vec2 old = chroma_texel(&w->frames.a, chroma, p);
    vec2 new = chroma_texel(&w->frames.b, chroma, p);
    quad_store_chroma(w, p, v2(mixf(old.x, new.x, s.weight), mixf(old.y, new.y, s.weight)));
}

void ref_wipe_composite(const mb_wipe_composite_pass *pass)
{
    const mb_warp *w = &pass->warp;
    if (!wipe_gate_open(pass->gate, MB_WIPE_GATE_CELLS, MB_WIPE_MIN_CELLS)) {
        return;
    }
    for (int y = 0; y < w->height; y++) {
        for (int x = 0; x < w->width; x++) {
            compose_luma(pass, iv2(x, y));
        }
    }
    for (int y = 0; y < w->chroma.height; y++) {
        for (int x = 0; x < w->chroma.width; x++) {
            compose_chroma(pass, iv2(x, y));
        }
    }
}
