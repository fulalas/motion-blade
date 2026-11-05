#include <math.h>

#include "common/blocks.h"
#include "estimation/window.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"
#include "selection/select_context.h"

static void add_projected(select_context *ctx)
{
    const mb_select_pass *pass = ctx->pass;
    const uint32_t *keys = &words(pass->keys)[ctx->id * MB_KEYS_PER_BLOCK];
    for (int i = 0; i < MB_KEYS_PER_BLOCK && keys[i] != MB_KEY_NONE; i++) {
        int source = (int)(keys[i] >> 1);
        bool backward = (keys[i] & 1u) != 0;
        vec2 u = backward ? v2_neg(v4_xy(vec4s(pass->backward)[source])) : v4_xy(vec4s(pass->forward)[source]);
        select_add(ctx, u);
    }
}

static bool any_fully_supported(const select_context *ctx)
{
    for (int i = 0; i < ctx->count; i++) {
        vec2 u = ctx->candidates[i];
        consistency ok = select_consistency(ctx, u);
        if (ctx->has_previous && ctx->has_next && ok.a && ok.b && select_supported_both(ctx, u)) {
            return true;
        }
    }
    return false;
}

static bool extra_is_trusted(const select_context *ctx, vec2 u, bool from_next)
{
    float limit = consistency_limit(ctx->pass->consistency, u);
    vec2 errors = select_consistency_errors(ctx, u);
    bool own = (from_next ? errors.y : errors.x) <= limit;
    return ctx->has_previous && ctx->has_next && (own || select_supported_both(ctx, u));
}

static void add_extra(select_context *ctx)
{
    const mb_select_pass *pass = ctx->pass;
    const uint32_t *keys = &words(pass->extra_keys)[ctx->id * MB_KEYS_PER_BLOCK];
    if (keys[0] == MB_KEY_NONE || any_fully_supported(ctx)) {
        return;
    }
    for (int i = 0; i < 2 && keys[i] != MB_KEY_NONE; i++) {
        int source = (int)(keys[i] >> 1);
        bool from_next = (keys[i] & 1u) != 0;
        vec2 u = from_next ? v4_xy(vec4s(pass->next_forward)[source])
                           : v2_neg(v4_xy(vec4s(pass->previous_backward)[source]));
        if (extra_is_trusted(ctx, u, from_next)) {
            select_add(ctx, u);
        }
    }
}

static void find_agreed_vector(select_context *ctx)
{
    const mb_select_pass *pass = ctx->pass;
    ctx->agreed = false;
    ctx->agreed_vector = v2(0.0f, 0.0f);
    if (pass->support.penalty_disagree <= 0.0f || !ctx->has_previous || !ctx->has_next) {
        return;
    }
    vec2 choices[3] = {
        v2(0.0f, 0.0f),
        v4_xy(vec4s(pass->next_forward)[ctx->id]),
        v2_neg(v4_xy(vec4s(pass->previous_backward)[ctx->id])),
    };
    for (int k = 0; k < 3 && !ctx->agreed; k++) {
        ctx->agreed = select_supported_both(ctx, choices[k]);
        ctx->agreed_vector = choices[k];
    }
}

static void add_ring(select_context *ctx)
{
    const mb_select_pass *pass = ctx->pass;
    for (int k = 0; k < 4; k++) {
        int q = block_index(iv2_add(ctx->block, RING8[k]), pass->grid);
        select_add(ctx, v4_xy(vec4s(pass->forward)[q]));
        select_add(ctx, v2_neg(v4_xy(vec4s(pass->backward)[q])));
    }
}

static float neighbour_dc(const select_context *ctx)
{
    const mb_select_pass *pass = ctx->pass;
    float smallest = 1e30f;
    for (int k = 0; k < 9; k++) {
        int q = neighbour3_index(k, ctx->block, pass->grid);
        smallest = fminf(smallest, fmaxf(fabsf(vec4s(pass->forward)[q].w), fabsf(vec4s(pass->backward)[q].w)));
    }
    return smallest;
}

static bool any_long_supported(const select_context *ctx)
{
    if (!ctx->pass->support.enabled || !ctx->has_previous || !ctx->has_next) {
        return false;
    }
    for (int i = 0; i < ctx->two_sided_count; i++) {
        vec2 u = ctx->candidates[i];
        if (v2_l1(u) >= ctx->pass->support.min_length && select_supported_both(ctx, u)) {
            return true;
        }
    }
    return false;
}

static void begin_block(select_context *ctx, const mb_select_pass *pass, int id)
{
    select_context_begin(ctx, pass, id);
    add_projected(ctx);
    select_add(ctx, v4_xy(vec4s(pass->forward)[id]));
    select_add(ctx, v2_neg(v4_xy(vec4s(pass->backward)[id])));
    add_extra(ctx);
    find_agreed_vector(ctx);
    ctx->two_sided_count = ctx->count;
    add_ring(ctx);
    ctx->mean_weight = mean_weight(pass->mean_removal, neighbour_dc(ctx));
    ctx->any_supported = any_long_supported(ctx);
}

static vec4 select_block(const mb_select_pass *pass, int id)
{
    select_context ctx;
    begin_block(&ctx, pass, id);
    select_score_two_sided(&ctx);
    select_check_agreed(&ctx);
    select_refine_supported(&ctx);
    select_try_uncover(&ctx);
    select_try_one_sided(&ctx);
    return (vec4){ctx.best.vector.x, ctx.best.vector.y, (float)ctx.best.mode, block_dc(ctx.best.diff_sum)};
}

void ref_select(const mb_select_pass *pass)
{
    vec4 *selection = vec4s(pass->selection);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        selection[id] = select_block(pass, id);
    }
}
