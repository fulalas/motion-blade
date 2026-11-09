#ifndef MOTIONBLADE_PARAMS_H
#define MOTIONBLADE_PARAMS_H

#include "motionblade/status.h"

typedef struct mb_search_params {
    int full_search_radius;
    float full_search_length_penalty;
    float penalty_parent;
    float penalty_temporal;
    float penalty_random;
    float penalty_zero;
    float penalty_reverse;
    float penalty_length;
    float mean_removal_lo;
    float mean_removal_hi;
    float early_exit;
    int iterations;
    int base_iterations;
    int half_resolution;
} mb_search_params;

typedef struct mb_repair_params {
    int cross_check_directions;
    float cross_penalty;
    float max_cost_ratio;
    float min_cost_gain;
    float cross_min_change;
    int local_search;
    int local_time_check;
    float local_min_cost;
    float local_noise_factor;
    float local_radius;
    float local_step;
} mb_repair_params;

typedef struct mb_cut_params {
    float max_threshold;
    float error_ratio;
    float zero_mean_ratio;
    float zero_mean_min;
    float spread_norm;
    float spread_min;
} mb_cut_params;

typedef struct mb_select_params {
    float consistency_threshold;
    float penalty_inconsistent;
    float foreground_bonus;
    int temporal_candidates;
    int temporal_support;
    float support_rel;
    float support_abs;
    float penalty_unsupported;
    float support_min_length;
    int support_refine_radius;
    int support_waive;
    float penalty_disagree;
    float one_sided_margin;
    float one_sided_ratio;
    float one_sided_min_cost;
    float one_sided_texture;
    float occlusion_strip;
    int uncover;
    float uncover_min_cost;
    float uncover_continuity;
    float median_threshold;
    float split_threshold;
    int split_refine_radius;
    float split_refine_penalty;
    float fallback_spread;
    float fallback_lo;
    float fallback_hi;
} mb_select_params;

typedef struct mb_blend_params {
    float softness;
    float penalty_one_sided;
    float penalty_unconfirmed;
    float penalty_fallback;
    float penalty_fallback_uncover;
    float candidate_prior;
    float foreground_bonus;
    float nearest_side_lo;
    float nearest_side_hi;
    float unsure_spread_lo;
    float unsure_spread_hi;
    float unsure_margin_lo;
    float unsure_margin_hi;
} mb_blend_params;

typedef struct mb_panel_params {
    float change_lo;
    float change_hi;
    float flat;
    float saturation;
    float switch_level;
    float source_distance;
    int radius;
} mb_panel_params;

typedef struct mb_framing_params {
    int crop_bars;
    int settle_frames;
} mb_framing_params;

typedef struct mb_params {
    mb_search_params search;
    mb_repair_params repair;
    mb_cut_params cut;
    mb_select_params select;
    mb_blend_params blend;
    mb_panel_params panel;
    mb_framing_params framing;
} mb_params;

mb_params mb_params_default(void);
mb_status mb_params_set(mb_params *params, const char *name, double value);

#endif
