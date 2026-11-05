#ifndef MOTIONBLADE_PASSES_H
#define MOTIONBLADE_PASSES_H

#include "motionblade/resources.h"

typedef struct mb_normalize_pass {
    mb_image *source;
    mb_image *target;
    int width;
    int height;
    float scale;
} mb_normalize_pass;

typedef struct mb_downsample_pass {
    mb_image *source;
    mb_image *target;
    int width;
    int height;
    int source_width;
    int source_height;
} mb_downsample_pass;

typedef struct mb_full_search_pass {
    mb_image *origin;
    mb_image *target;
    mb_buffer *field;
    mb_grid grid;
    int radius;
    float length_penalty;
} mb_full_search_pass;

typedef struct mb_search_penalties {
    float parent;
    float temporal;
    float random;
    float zero;
    float reverse;
    float length;
} mb_search_penalties;

typedef struct mb_mean_removal {
    float lo;
    float hi;
} mb_mean_removal;

typedef struct mb_iterate_pass {
    mb_image *origin;
    mb_image *target;
    mb_buffer *previous;
    mb_buffer *field;
    mb_buffer *parent;
    mb_buffer *temporal;
    mb_buffer *reverse;
    mb_buffer *history;
    mb_grid grid;
    int level;
    int parent_blocks_x;
    int parent_blocks_y;
    uint32_t seed;
    mb_search_penalties penalties;
    mb_mean_removal mean_removal;
    float early_exit;
} mb_iterate_pass;

typedef struct mb_measure_pass {
    mb_image *origin;
    mb_image *target;
    mb_buffer *field;
    mb_buffer *stats;
    mb_grid grid;
    bool accumulate_error;
    int direction;
    mb_mean_removal mean_removal;
} mb_measure_pass;

typedef struct mb_project_pass {
    mb_buffer *field;
    mb_buffer *keys;
    mb_grid grid;
    float time_scale;
    uint32_t tag;
} mb_project_pass;

typedef struct mb_acceptance {
    float max_ratio;
    float min_gain;
} mb_acceptance;

typedef struct mb_cross_check_pass {
    mb_image *origin;
    mb_image *target;
    mb_buffer *field;
    mb_buffer *repaired;
    mb_buffer *other;
    mb_buffer *keys;
    mb_grid grid;
    float penalty;
    float length_penalty;
    float min_change;
    mb_acceptance acceptance;
} mb_cross_check_pass;

typedef struct mb_support_tolerance {
    float rel;
    float abs;
} mb_support_tolerance;

typedef struct mb_local_search_pass {
    mb_image *origin;
    mb_image *target;
    mb_buffer *field;
    mb_buffer *repaired;
    mb_buffer *reference;
    mb_buffer *stats;
    mb_grid grid;
    bool reference_at_landing;
    float length_penalty;
    float noise_factor;
    float min_cost;
    float radius;
    float step;
    mb_support_tolerance support;
    mb_acceptance acceptance;
} mb_local_search_pass;

typedef struct mb_cut_pass {
    mb_buffer *stats;
    mb_buffer *history;
    int block_count;
    float error_scale;
    float max_threshold;
    float error_ratio;
    float zero_mean_ratio;
    float zero_mean_min;
    float spread_norm;
    float spread_min;
} mb_cut_pass;

typedef struct mb_one_sided_rules {
    float margin;
    float ratio;
    float min_cost;
    float texture;
    float strip;
} mb_one_sided_rules;

typedef struct mb_uncover_rules {
    bool enabled;
    float min_cost;
    float continuity;
} mb_uncover_rules;

typedef struct mb_support_rules {
    bool enabled;
    mb_support_tolerance tolerance;
    float penalty_unsupported;
    float min_length;
    int refine_radius;
    bool waive;
    float penalty_disagree;
} mb_support_rules;

typedef struct mb_select_frames {
    mb_image *a;
    mb_image *b;
    mb_image *c;
    mb_image *z;
} mb_select_frames;

typedef struct mb_select_pass {
    mb_select_frames frames;
    mb_buffer *forward;
    mb_buffer *backward;
    mb_buffer *keys;
    mb_buffer *selection;
    mb_buffer *next_forward;
    mb_buffer *previous_backward;
    mb_buffer *next_stats;
    mb_buffer *previous_stats;
    mb_buffer *extra_keys;
    mb_grid grid;
    float t;
    bool has_next;
    bool has_previous;
    float penalty_inconsistent;
    float consistency;
    float foreground_bonus;
    mb_mean_removal mean_removal;
    mb_support_rules support;
    mb_one_sided_rules one_sided;
    mb_uncover_rules uncover;
} mb_select_pass;

typedef struct mb_median_pass {
    mb_buffer *selection;
    mb_buffer *filtered;
    mb_grid grid;
    float threshold;
} mb_median_pass;

typedef struct mb_fallback_pass {
    mb_buffer *selection;
    mb_buffer *weights;
    mb_buffer *forward;
    mb_buffer *backward;
    mb_grid grid;
    float t;
    float spread;
    float lo;
    float hi;
} mb_fallback_pass;

typedef struct mb_pair_planes {
    mb_frame_planes a;
    mb_frame_planes b;
} mb_pair_planes;

typedef struct mb_chroma_offset_pass {
    mb_pair_planes frames;
    mb_buffer *selection;
    mb_buffer *offsets;
    mb_grid grid;
    mb_chroma_layout chroma;
    int base_level;
    float t;
    float luma_scale;
} mb_chroma_offset_pass;

typedef struct mb_offset_median_pass {
    mb_buffer *offsets;
    mb_buffer *filtered;
    mb_grid grid;
} mb_offset_median_pass;

typedef struct mb_global_motion_pass {
    mb_buffer *selection;
    mb_buffer *global_motion;
    mb_grid grid;
} mb_global_motion_pass;

typedef struct mb_panel_pass {
    mb_pair_planes frames;
    mb_buffer *flags;
    mb_buffer *global_motion;
    mb_grid grid;
    int width;
    int height;
    mb_chroma_layout chroma;
    int base_level;
    float t;
    float level_scale;
    float change_lo;
    float change_hi;
    float flat;
    float chroma_mid;
    float saturation;
} mb_panel_pass;

typedef struct mb_panel_spread_pass {
    mb_buffer *flags;
    mb_buffer *spread;
    mb_buffer *selection;
    mb_buffer *global_motion;
    mb_grid grid;
    int radius;
    float source_distance;
    float t;
} mb_panel_spread_pass;

typedef struct mb_split_pass {
    mb_image *a;
    mb_image *b;
    mb_buffer *selection;
    mb_buffer *cells;
    mb_buffer *offsets;
    mb_grid grid;
    int refine_radius;
    float t;
    float threshold;
    float luma_scale;
    float refine_penalty;
} mb_split_pass;

typedef struct mb_blend_costs {
    float softness;
    float one_sided;
    float unconfirmed;
    float fallback;
    float fallback_uncover;
    float candidate_prior;
    float consistency;
    float foreground_bonus;
    float nearest_side_lo;
    float nearest_side_hi;
    float panel_switch;
} mb_blend_costs;

typedef struct mb_output_range {
    bool sixteen_bit;
    float max_value;
    float step;
} mb_output_range;

typedef struct mb_extrapolation_side {
    mb_buffer *field;
    mb_buffer *stats;
    mb_image *neighbour_level;
} mb_extrapolation_side;

typedef struct mb_extrapolation {
    mb_extrapolation_side past;
    mb_extrapolation_side next;
} mb_extrapolation;

typedef struct mb_warp {
    mb_pair_planes frames;
    mb_image *a_level;
    mb_image *b_level;
    mb_frame_planes output;
    mb_buffer *stats;
    int width;
    int height;
    mb_grid grid;
    mb_chroma_layout chroma;
    int base_level;
    float t;
    mb_output_range range;
} mb_warp;

typedef struct mb_interpolate_pass {
    mb_warp warp;
    mb_buffer *cells;
    mb_buffer *forward;
    mb_buffer *backward;
    mb_buffer *fallback;
    mb_buffer *offsets;
    mb_buffer *panels;
    mb_buffer *global_motion;
    float luma_scale;
    mb_blend_costs costs;
} mb_interpolate_pass;

typedef struct mb_extrapolate_pass {
    mb_warp warp;
    mb_extrapolation extrapolation;
    float softness;
    float unconfirmed;
    float consistency;
} mb_extrapolate_pass;

#endif
