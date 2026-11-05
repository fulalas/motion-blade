#include "vk_passes.h"

#include "push_constants.h"
#include "vk_pipelines.h"

enum { TILE = 16, QUAD_GROUP = 8 };

PUSH_FITS(struct normalize_push);
PUSH_FITS(struct downsample_push);
PUSH_FITS(struct full_search_push);
PUSH_FITS(struct iterate_push);
PUSH_FITS(struct measure_push);
PUSH_FITS(struct project_push);
PUSH_FITS(struct cross_check_push);
PUSH_FITS(struct local_search_push);
PUSH_FITS(struct cut_push);
PUSH_FITS(struct select_push);
PUSH_FITS(struct median_push);
PUSH_FITS(struct fallback_push);
PUSH_FITS(struct chroma_offsets_push);
PUSH_FITS(struct blocks_push);
PUSH_FITS(struct panels_push);
PUSH_FITS(struct panel_spread_push);
PUSH_FITS(struct split_push);
PUSH_FITS(struct interpolate_push);
PUSH_FITS(struct bar_flags_push);

static uint32_t tiles(int size)
{
    return (uint32_t)((size + TILE - 1) / TILE);
}

static void over_groups(mbvk_backend *vk, dispatch_desc desc, uint32_t groups_x, uint32_t groups_y)
{
    desc.groups_x = groups_x;
    desc.groups_y = groups_y;
    pipeline_dispatch(vk, &desc);
}

static void over_items(mbvk_backend *vk, dispatch_desc desc, int items)
{
    over_groups(vk, desc, pipeline_groups(desc.pass, items), 1);
}

static void over_tiles(mbvk_backend *vk, dispatch_desc desc, int width, int height)
{
    over_groups(vk, desc, tiles(width), tiles(height));
}

static binding image(const mb_image *image)
{
    return (binding){image, NULL};
}

static binding buffer(const mb_buffer *buffer)
{
    return (binding){NULL, buffer};
}

void vk_normalize(void *self, const mb_normalize_pass *pass)
{
    struct normalize_push push = {pass->width, pass->height, pass->scale};
    binding b[] = {image(pass->source), image(pass->target)};
    over_tiles(self, (dispatch_desc){PASS_NORMALIZE, b, PUSH(push)}, pass->width, pass->height);
}

void vk_downsample(void *self, const mb_downsample_pass *pass)
{
    struct downsample_push push = {pass->width, pass->height, pass->source_width, pass->source_height};
    binding b[] = {image(pass->source), image(pass->target)};
    over_tiles(self, (dispatch_desc){PASS_DOWNSAMPLE, b, PUSH(push)}, pass->width, pass->height);
}

void vk_full_search(void *self, const mb_full_search_pass *pass)
{
    mb_grid g = pass->grid;
    struct full_search_push push = {g.blocks_x, g.blocks_y, g.width, g.height, pass->radius, pass->length_penalty};
    binding b[] = {image(pass->origin), image(pass->target), buffer(pass->field)};
    over_items(self, (dispatch_desc){PASS_FULL_SEARCH, b, PUSH(push)}, mb_grid_block_count(g));
}

static const char *iterate_name(int level)
{
    static const char *const NAMES[MB_MAX_LEVELS] = {"iterate_L0", "iterate_L1", "iterate_L2",
                                                      "iterate_L3", "iterate_L4", "iterate_L5"};
    return NAMES[level];
}

void vk_iterate_search(void *self, const mb_iterate_pass *pass)
{
    mb_grid g = pass->grid;
    const mb_search_penalties *p = &pass->penalties;
    struct iterate_push push = {
        g.blocks_x, g.blocks_y, g.width, g.height, pass->parent_blocks_x, pass->parent_blocks_y,
        pass->previous != NULL, pass->parent != NULL, pass->temporal != NULL, pass->reverse != NULL, pass->seed,
        p->parent, p->temporal, p->random, p->zero, p->reverse, p->length, pass->early_exit, pass->mean_removal.lo,
        pass->mean_removal.hi,
    };
    binding b[] = {image(pass->origin),    image(pass->target),   buffer(pass->previous), buffer(pass->field),
                   buffer(pass->parent),   buffer(pass->temporal), buffer(pass->reverse), buffer(pass->history)};
    dispatch_desc desc = {PASS_ITERATE, b, PUSH(push), iterate_name(pass->level)};
    over_items(self, desc, mb_grid_block_count(g));
}

void vk_measure(void *self, const mb_measure_pass *pass)
{
    mb_grid g = pass->grid;
    struct measure_push push = {g.blocks_x, g.blocks_y, g.width, g.height, pass->accumulate_error, pass->direction,
                                pass->mean_removal.lo, pass->mean_removal.hi};
    binding b[] = {image(pass->origin), image(pass->target), buffer(pass->field), buffer(pass->stats)};
    over_items(self, (dispatch_desc){PASS_MEASURE, b, PUSH(push)}, mb_grid_block_count(g));
}

void vk_project(void *self, const mb_project_pass *pass)
{
    struct project_push push = {pass->grid.blocks_x, pass->grid.blocks_y, pass->time_scale, pass->tag};
    binding b[] = {buffer(pass->field), buffer(pass->keys)};
    over_items(self, (dispatch_desc){PASS_PROJECT, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_cross_check(void *self, const mb_cross_check_pass *pass)
{
    mb_grid g = pass->grid;
    struct cross_check_push push = {
        g.blocks_x, g.blocks_y, g.width, g.height, pass->penalty, pass->length_penalty, pass->min_change,
        pass->acceptance.max_ratio, pass->acceptance.min_gain,
    };
    binding b[] = {image(pass->origin), image(pass->target), buffer(pass->field),
                   buffer(pass->repaired), buffer(pass->other), buffer(pass->keys)};
    over_items(self, (dispatch_desc){PASS_CROSS_CHECK, b, PUSH(push)}, mb_grid_block_count(g));
}

void vk_local_search(void *self, const mb_local_search_pass *pass)
{
    mb_grid g = pass->grid;
    struct local_search_push push = {
        g.blocks_x, g.blocks_y, g.width, g.height, pass->reference != NULL, pass->reference_at_landing,
        pass->length_penalty, pass->noise_factor, pass->min_cost, pass->radius, pass->step, pass->support.rel,
        pass->support.abs, pass->acceptance.max_ratio, pass->acceptance.min_gain,
    };
    binding b[] = {image(pass->origin), image(pass->target), buffer(pass->field),
                   buffer(pass->repaired), buffer(pass->reference), buffer(pass->stats)};
    over_items(self, (dispatch_desc){PASS_LOCAL_SEARCH, b, PUSH(push)}, mb_grid_block_count(g));
}

void vk_decide_cut(void *self, const mb_cut_pass *pass)
{
    struct cut_push push = {pass->block_count, pass->error_scale, pass->max_threshold, pass->error_ratio,
                            pass->zero_mean_ratio, pass->zero_mean_min, pass->spread_norm, pass->spread_min};
    binding b[] = {buffer(pass->stats), buffer(pass->history)};
    over_items(self, (dispatch_desc){PASS_DECIDE_CUT, b, PUSH(push)}, 1);
}

static struct select_push select_constants(const mb_select_pass *pass)
{
    mb_grid g = pass->grid;
    const mb_support_rules *s = &pass->support;
    const mb_one_sided_rules *o = &pass->one_sided;
    return (struct select_push){
        g.blocks_x, g.blocks_y, g.width, g.height, pass->has_next, pass->has_previous, s->enabled, s->refine_radius,
        s->waive, pass->uncover.enabled, pass->t, pass->penalty_inconsistent, pass->consistency,
        pass->foreground_bonus, s->tolerance.rel, s->tolerance.abs, s->penalty_unsupported, s->min_length,
        s->penalty_disagree, o->margin, o->ratio, o->min_cost, o->texture, o->strip, pass->uncover.min_cost,
        pass->uncover.continuity, pass->mean_removal.lo, pass->mean_removal.hi,
    };
}

void vk_select(void *self, const mb_select_pass *pass)
{
    struct select_push push = select_constants(pass);
    const mb_select_frames *f = &pass->frames;
    binding b[] = {image(f->a),
                   image(f->b),
                   image(f->c),
                   image(f->z),
                   buffer(pass->forward),
                   buffer(pass->backward),
                   buffer(pass->keys),
                   buffer(pass->selection),
                   buffer(pass->next_forward),
                   buffer(pass->previous_backward),
                   buffer(pass->next_stats),
                   buffer(pass->previous_stats),
                   buffer(pass->extra_keys)};
    over_items(self, (dispatch_desc){PASS_SELECT, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_median(void *self, const mb_median_pass *pass)
{
    struct median_push push = {pass->grid.blocks_x, pass->grid.blocks_y, pass->threshold};
    binding b[] = {buffer(pass->selection), buffer(pass->filtered)};
    over_items(self, (dispatch_desc){PASS_MEDIAN, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_fallback(void *self, const mb_fallback_pass *pass)
{
    struct fallback_push push = {pass->grid.blocks_x, pass->grid.blocks_y, pass->t, pass->spread, pass->lo, pass->hi};
    binding b[] = {buffer(pass->selection), buffer(pass->weights), buffer(pass->forward), buffer(pass->backward)};
    over_items(self, (dispatch_desc){PASS_FALLBACK, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

static mb_image *second_chroma(const mb_frame_planes *planes)
{
    return planes->chroma[1] ? planes->chroma[1] : planes->chroma[0];
}

void vk_chroma_offsets(void *self, const mb_chroma_offset_pass *pass)
{
    const mb_chroma_layout *c = &pass->chroma;
    struct chroma_offsets_push push = {
        pass->grid.blocks_x, pass->grid.blocks_y, c->width, c->height, c->shift_x, c->shift_y, c->interleaved,
        pass->base_level, pass->t, pass->luma_scale,
    };
    const mb_pair_planes *f = &pass->frames;
    binding b[] = {image(f->a.chroma[0]), image(f->b.chroma[0]), image(second_chroma(&f->a)),
                   image(second_chroma(&f->b)), buffer(pass->selection), buffer(pass->offsets)};
    over_items(self, (dispatch_desc){PASS_CHROMA_OFFSETS, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_offset_median(void *self, const mb_offset_median_pass *pass)
{
    struct blocks_push push = {pass->grid.blocks_x, pass->grid.blocks_y};
    binding b[] = {buffer(pass->offsets), buffer(pass->filtered)};
    over_items(self, (dispatch_desc){PASS_OFFSET_MEDIAN, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_global_motion(void *self, const mb_global_motion_pass *pass)
{
    struct blocks_push push = {pass->grid.blocks_x, pass->grid.blocks_y};
    binding b[] = {buffer(pass->selection), buffer(pass->global_motion)};
    over_items(self, (dispatch_desc){PASS_GLOBAL_MOTION, b, PUSH(push)}, 1);
}

void vk_panels(void *self, const mb_panel_pass *pass)
{
    const mb_chroma_layout *c = &pass->chroma;
    struct panels_push push = {
        pass->grid.blocks_x, pass->grid.blocks_y, pass->width, pass->height, c->width, c->height, c->shift_x,
        c->shift_y, c->interleaved, pass->base_level, pass->t, pass->level_scale, pass->change_lo, pass->change_hi,
        pass->flat, pass->chroma_mid, pass->saturation,
    };
    const mb_pair_planes *f = &pass->frames;
    binding b[] = {image(f->a.luma),           image(f->b.luma),           image(f->a.chroma[0]),
                   image(f->b.chroma[0]),      image(second_chroma(&f->a)), image(second_chroma(&f->b)),
                   buffer(pass->flags),        buffer(pass->global_motion)};
    over_items(self, (dispatch_desc){PASS_PANELS, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_panel_spread(void *self, const mb_panel_spread_pass *pass)
{
    struct panel_spread_push push = {pass->grid.blocks_x, pass->grid.blocks_y, pass->radius, pass->source_distance,
                                     pass->t};
    binding b[] = {buffer(pass->flags), buffer(pass->spread), buffer(pass->selection), buffer(pass->global_motion)};
    over_items(self, (dispatch_desc){PASS_PANEL_SPREAD, b, PUSH(push)}, mb_grid_block_count(pass->grid));
}

void vk_split(void *self, const mb_split_pass *pass)
{
    mb_grid g = pass->grid;
    struct split_push push = {g.blocks_x, g.blocks_y, g.width, g.height, pass->refine_radius, pass->t, pass->threshold,
                              pass->luma_scale, pass->refine_penalty};
    binding b[] = {image(pass->a), image(pass->b), buffer(pass->selection), buffer(pass->cells), buffer(pass->offsets)};
    over_items(self, (dispatch_desc){PASS_SPLIT, b, PUSH(push)}, mb_grid_block_count(g) * 4);
}

static pass_id interpolate_variant(const mb_interpolate_pass *pass)
{
    if (pass->chroma.interleaved) {
        return pass->range.sixteen_bit ? PASS_INTERPOLATE_16_NV : PASS_INTERPOLATE_8_NV;
    }
    return pass->range.sixteen_bit ? PASS_INTERPOLATE_16 : PASS_INTERPOLATE_8;
}

static struct interpolate_push interpolate_constants(const mb_interpolate_pass *pass)
{
    const mb_blend_costs *k = &pass->costs;
    const mb_chroma_layout *c = &pass->chroma;
    return (struct interpolate_push){
        pass->width, pass->height, pass->grid.blocks_x, pass->grid.blocks_y, c->width, c->height, c->shift_x,
        c->shift_y, pass->base_level, pass->t, k->softness, k->one_sided, k->unconfirmed, k->fallback,
        k->candidate_prior, k->consistency, k->fallback_uncover, pass->luma_scale, pass->range.max_value,
        pass->range.step, k->foreground_bonus, k->nearest_side_lo, k->nearest_side_hi, k->panel_switch,
        (pass->extrapolation.past.field != NULL) | (pass->extrapolation.next.field != NULL) << 1,
    };
}

static uint32_t quad_groups(int size)
{
    return (uint32_t)(((size + 1) / 2 + QUAD_GROUP - 1) / QUAD_GROUP);
}

void vk_interpolate(void *self, const mb_interpolate_pass *pass)
{
    struct interpolate_push push = interpolate_constants(pass);
    const mb_pair_planes *f = &pass->frames;
    const mb_frame_planes *o = &pass->output;
    binding b[] = {image(f->a.luma),          image(f->b.luma),          image(f->a.chroma[0]),
                   image(f->b.chroma[0]),     image(second_chroma(&f->a)), image(second_chroma(&f->b)),
                   buffer(pass->cells),       buffer(pass->forward),     buffer(pass->backward),
                   image(o->luma),            image(o->chroma[0]),       image(second_chroma(o)),
                   image(pass->a_level),      image(pass->b_level),      buffer(pass->stats),
                   buffer(pass->fallback),    buffer(pass->offsets),     buffer(pass->panels),
                   buffer(pass->global_motion),
                   buffer(pass->extrapolation.past.field), buffer(pass->extrapolation.past.stats),
                   buffer(pass->extrapolation.next.field), buffer(pass->extrapolation.next.stats),
                   image(pass->extrapolation.past.neighbour_level), image(pass->extrapolation.next.neighbour_level)};
    over_groups(self, (dispatch_desc){interpolate_variant(pass), b, PUSH(push)}, quad_groups(pass->width),
                quad_groups(pass->height));
}
