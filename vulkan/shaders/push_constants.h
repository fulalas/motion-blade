#ifndef MBVK_PUSH_CONSTANTS_H
#define MBVK_PUSH_CONSTANTS_H

#ifdef GL_core_profile
#define int32_t int
#define uint32_t uint
#else
#include <stdint.h>
#endif

struct normalize_push {
    int32_t width;
    int32_t height;
    float scale;
};

struct downsample_push {
    int32_t width;
    int32_t height;
    int32_t source_width;
    int32_t source_height;
};

struct full_search_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t radius;
    float length_penalty;
};

struct iterate_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t parent_blocks_x;
    int32_t parent_blocks_y;
    int32_t has_previous;
    int32_t has_parent;
    int32_t has_temporal;
    int32_t has_reverse;
    uint32_t seed;
    float penalty_parent;
    float penalty_temporal;
    float penalty_random;
    float penalty_zero;
    float penalty_reverse;
    float penalty_length;
    float early_exit;
    float mean_lo;
    float mean_hi;
};

struct measure_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t accumulate_error;
    int32_t direction;
    float mean_lo;
    float mean_hi;
};

struct project_push {
    int32_t blocks_x;
    int32_t blocks_y;
    float time_scale;
    uint32_t tag;
};

struct cross_check_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    float penalty;
    float length_penalty;
    float min_change;
    float max_ratio;
    float min_gain;
};

struct local_search_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t has_reference;
    int32_t reference_at_landing;
    float length_penalty;
    float noise_factor;
    float min_cost;
    float radius;
    float step;
    float support_rel;
    float support_abs;
    float max_ratio;
    float min_gain;
};

struct cut_push {
    int32_t block_count;
    float error_scale;
    float max_threshold;
    float error_ratio;
    float zero_mean_ratio;
    float zero_mean_min;
    float spread_norm;
    float spread_min;
};

struct select_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t has_next;
    int32_t has_previous;
    int32_t support_enabled;
    int32_t refine_radius;
    int32_t waive;
    int32_t uncover;
    float t;
    float penalty_inconsistent;
    float consistency;
    float foreground_bonus;
    float support_rel;
    float support_abs;
    float penalty_unsupported;
    float support_min_length;
    float penalty_disagree;
    float one_sided_margin;
    float one_sided_ratio;
    float one_sided_min_cost;
    float one_sided_texture;
    float occlusion_strip;
    float uncover_min_cost;
    float uncover_continuity;
    float mean_lo;
    float mean_hi;
};

struct median_push {
    int32_t blocks_x;
    int32_t blocks_y;
    float threshold;
};

struct fallback_push {
    int32_t blocks_x;
    int32_t blocks_y;
    float t;
    float spread;
    float lo;
    float hi;
};

struct chroma_offsets_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t chroma_width;
    int32_t chroma_height;
    int32_t shift_x;
    int32_t shift_y;
    int32_t interleaved;
    int32_t base_level;
    float t;
    float luma_scale;
};

struct blocks_push {
    int32_t blocks_x;
    int32_t blocks_y;
};

struct panels_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t chroma_width;
    int32_t chroma_height;
    int32_t shift_x;
    int32_t shift_y;
    int32_t interleaved;
    int32_t base_level;
    float t;
    float level_scale;
    float change_lo;
    float change_hi;
    float flat_range;
    float chroma_mid;
    float saturation;
};

struct panel_spread_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t radius;
    float source_distance;
    float t;
};

struct split_push {
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t width;
    int32_t height;
    int32_t refine_radius;
    float t;
    float threshold;
    float luma_scale;
    float refine_penalty;
};

struct interpolate_push {
    int32_t width;
    int32_t height;
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t chroma_width;
    int32_t chroma_height;
    int32_t shift_x;
    int32_t shift_y;
    int32_t base_level;
    float t;
    float softness;
    float penalty_one_sided;
    float penalty_unconfirmed;
    float penalty_fallback;
    float candidate_prior;
    float consistency;
    float penalty_fallback_uncover;
    float luma_scale;
    float max_value;
    float quant_step;
    float foreground_bonus;
    float nearest_lo;
    float nearest_hi;
    float panel_switch;
    float unsure_spread_lo;
    float unsure_spread_hi;
    float unsure_margin_lo;
    float unsure_margin_hi;
};

struct extrapolate_push {
    int32_t width;
    int32_t height;
    int32_t blocks_x;
    int32_t blocks_y;
    int32_t chroma_width;
    int32_t chroma_height;
    int32_t shift_x;
    int32_t shift_y;
    int32_t base_level;
    float t;
    float softness;
    float penalty_unconfirmed;
    float consistency;
    float max_value;
    float quant_step;
    int32_t extrapolate;
};

struct bar_flags_push {
    int32_t width;
    int32_t height;
    int32_t shift_x;
    int32_t shift_y;
    float luma_max;
    float chroma_tolerance;
};

#endif
