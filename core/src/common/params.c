#include "motionblade/params.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum { PARAM_INT, PARAM_FLOAT } param_type;

typedef struct param_entry {
    const char *name;
    size_t offset;
    param_type type;
    double initial;
    double min;
    double max;
} param_entry;

#define ANY -DBL_MAX, DBL_MAX
#define INT_PARAM(name, field, initial, ...) {name, offsetof(mb_params, field), PARAM_INT, initial, __VA_ARGS__}
#define FLOAT_PARAM(name, field, initial, ...) {name, offsetof(mb_params, field), PARAM_FLOAT, initial, __VA_ARGS__}

static const param_entry PARAM_TABLE[] = {
    INT_PARAM("full_search_radius", search.full_search_radius, 16, 0, 16),
    FLOAT_PARAM("full_search_length_penalty", search.full_search_length_penalty, 2, ANY),
    FLOAT_PARAM("penalty_parent", search.penalty_parent, 4, ANY),
    FLOAT_PARAM("penalty_temporal", search.penalty_temporal, 8, ANY),
    FLOAT_PARAM("penalty_random", search.penalty_random, 16, ANY),
    FLOAT_PARAM("penalty_zero", search.penalty_zero, 8, ANY),
    FLOAT_PARAM("penalty_reverse", search.penalty_reverse, 4, ANY),
    FLOAT_PARAM("penalty_length", search.penalty_length, 4, ANY),
    FLOAT_PARAM("mean_removal_lo", search.mean_removal_lo, 0.5f, 0, 1),
    FLOAT_PARAM("mean_removal_hi", search.mean_removal_hi, 1, 0, 1),
    FLOAT_PARAM("early_exit", search.early_exit, 0.06f, ANY),
    INT_PARAM("iterations", search.iterations, 3, 0, 16),
    INT_PARAM("base_iterations", search.base_iterations, 1, 1, 16),
    INT_PARAM("half_resolution", search.half_resolution, -1, -1, 1),
    INT_PARAM("cross_check_directions", repair.cross_check_directions, 2, 0, 2),
    FLOAT_PARAM("cross_penalty", repair.cross_penalty, 4, ANY),
    FLOAT_PARAM("max_cost_ratio", repair.max_cost_ratio, 0.7f, ANY),
    FLOAT_PARAM("min_cost_gain", repair.min_cost_gain, 400, ANY),
    FLOAT_PARAM("cross_min_change", repair.cross_min_change, 4, ANY),
    INT_PARAM("local_search", repair.local_search, 1, 0, 1),
    INT_PARAM("local_time_check", repair.local_time_check, 1, 0, 1),
    FLOAT_PARAM("local_min_cost", repair.local_min_cost, 1500, ANY),
    FLOAT_PARAM("local_noise_factor", repair.local_noise_factor, 1.5f, ANY),
    FLOAT_PARAM("local_radius", repair.local_radius, 4, 0, 64),
    FLOAT_PARAM("local_step", repair.local_step, 2, 0.25, 16),
    FLOAT_PARAM("cut_max_threshold", cut.max_threshold, 40, ANY),
    FLOAT_PARAM("cut_error_ratio", cut.error_ratio, 4, ANY),
    FLOAT_PARAM("cut_zero_mean_ratio", cut.zero_mean_ratio, 2, ANY),
    FLOAT_PARAM("cut_zero_mean_min", cut.zero_mean_min, 1, ANY),
    FLOAT_PARAM("cut_spread_norm", cut.spread_norm, 45, ANY),
    FLOAT_PARAM("cut_spread_min", cut.spread_min, 0.1f, ANY),
    FLOAT_PARAM("consistency_threshold", select.consistency_threshold, 8, ANY),
    FLOAT_PARAM("penalty_inconsistent", select.penalty_inconsistent, 256, ANY),
    FLOAT_PARAM("foreground_bonus", select.foreground_bonus, 1024, ANY),
    INT_PARAM("temporal_candidates", select.temporal_candidates, 1, 0, 1),
    INT_PARAM("temporal_support", select.temporal_support, 1, 0, 1),
    FLOAT_PARAM("support_rel", select.support_rel, 0.25f, ANY),
    FLOAT_PARAM("support_abs", select.support_abs, 4, ANY),
    FLOAT_PARAM("penalty_unsupported", select.penalty_unsupported, 512, ANY),
    FLOAT_PARAM("support_min_length", select.support_min_length, 8, ANY),
    INT_PARAM("support_refine_radius", select.support_refine_radius, 8, 0, 16),
    INT_PARAM("support_waive", select.support_waive, 1, 0, 1),
    FLOAT_PARAM("penalty_disagree", select.penalty_disagree, 2048, ANY),
    FLOAT_PARAM("one_sided_margin", select.one_sided_margin, 512, ANY),
    FLOAT_PARAM("one_sided_ratio", select.one_sided_ratio, 0.5f, ANY),
    FLOAT_PARAM("one_sided_min_cost", select.one_sided_min_cost, 2048, ANY),
    FLOAT_PARAM("one_sided_texture", select.one_sided_texture, 0.75f, ANY),
    FLOAT_PARAM("occlusion_strip", select.occlusion_strip, 4, ANY),
    INT_PARAM("uncover", select.uncover, 1, 0, 1),
    FLOAT_PARAM("uncover_min_cost", select.uncover_min_cost, 4000, ANY),
    FLOAT_PARAM("uncover_continuity", select.uncover_continuity, 0.5f, ANY),
    FLOAT_PARAM("median_threshold", select.median_threshold, 8, ANY),
    FLOAT_PARAM("split_threshold", select.split_threshold, 2, ANY),
    INT_PARAM("split_refine_radius", select.split_refine_radius, 2, 0, 8),
    FLOAT_PARAM("split_refine_penalty", select.split_refine_penalty, 16, ANY),
    FLOAT_PARAM("fallback_spread", select.fallback_spread, 0.7f, ANY),
    FLOAT_PARAM("fallback_lo", select.fallback_lo, 2, ANY),
    FLOAT_PARAM("fallback_hi", select.fallback_hi, 6, ANY),
    FLOAT_PARAM("blend_softness", blend.softness, 250, ANY),
    FLOAT_PARAM("penalty_one_sided", blend.penalty_one_sided, 384, ANY),
    FLOAT_PARAM("penalty_unconfirmed", blend.penalty_unconfirmed, 256, ANY),
    FLOAT_PARAM("penalty_fallback", blend.penalty_fallback, 1e6f, ANY),
    FLOAT_PARAM("penalty_fallback_uncover", blend.penalty_fallback_uncover, 768, ANY),
    FLOAT_PARAM("candidate_prior", blend.candidate_prior, 0.25f, ANY),
    FLOAT_PARAM("foreground_bonus_pixel", blend.foreground_bonus, 60, ANY),
    FLOAT_PARAM("nearest_side_lo", blend.nearest_side_lo, 8, ANY),
    FLOAT_PARAM("nearest_side_hi", blend.nearest_side_hi, 24, ANY),
    FLOAT_PARAM("unsure_spread_lo", blend.unsure_spread_lo, 12, ANY),
    FLOAT_PARAM("unsure_spread_hi", blend.unsure_spread_hi, 32, ANY),
    FLOAT_PARAM("unsure_margin_lo", blend.unsure_margin_lo, 100, ANY),
    FLOAT_PARAM("unsure_margin_hi", blend.unsure_margin_hi, 600, ANY),
    FLOAT_PARAM("panel_change_lo", panel.change_lo, 24, ANY),
    FLOAT_PARAM("panel_change_hi", panel.change_hi, 48, ANY),
    FLOAT_PARAM("panel_flat", panel.flat, 32, ANY),
    FLOAT_PARAM("panel_saturation", panel.saturation, 48, ANY),
    FLOAT_PARAM("panel_switch", panel.switch_level, 16, ANY),
    FLOAT_PARAM("panel_source_distance", panel.source_distance, 8, ANY),
    INT_PARAM("panel_radius", panel.radius, 8, 0, 16),
    INT_PARAM("crop_bars", framing.crop_bars, 1, 0, 1),
    INT_PARAM("bar_settle_frames", framing.settle_frames, 48, 0, 100000),
};

static const size_t PARAM_COUNT = sizeof(PARAM_TABLE) / sizeof(PARAM_TABLE[0]);

static void store_param(mb_params *params, const param_entry *entry, double value)
{
    char *field = (char *)params + entry->offset;
    if (entry->type == PARAM_INT) {
        int stored = (int)value;
        memcpy(field, &stored, sizeof(stored));
    } else {
        float stored = (float)value;
        memcpy(field, &stored, sizeof(stored));
    }
}

mb_params mb_params_default(void)
{
    mb_params params = {0};
    for (size_t i = 0; i < PARAM_COUNT; i++) {
        store_param(&params, &PARAM_TABLE[i], PARAM_TABLE[i].initial);
    }
    return params;
}

static const param_entry *find_param(const char *name)
{
    for (size_t i = 0; i < PARAM_COUNT; i++) {
        if (!strcmp(PARAM_TABLE[i].name, name)) {
            return &PARAM_TABLE[i];
        }
    }
    return NULL;
}

static bool mean_removal_ordered(const mb_params *params)
{
    return params->search.mean_removal_lo < params->search.mean_removal_hi;
}

mb_status mb_params_set(mb_params *params, const char *name, double value)
{
    const param_entry *entry = find_param(name);
    if (!entry || value < entry->min || value > entry->max) {
        return MB_ERROR_INVALID;
    }
    mb_params changed = *params;
    store_param(&changed, entry, value);
    if (!mean_removal_ordered(&changed)) {
        return MB_ERROR_INVALID;
    }
    *params = changed;
    return MB_SUCCESS;
}
