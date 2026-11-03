#ifndef MB_QUAD_H
#define MB_QUAD_H

#include "common/vec.h"
#include "motionblade/passes.h"

enum { QUAD_MAX_CANDIDATES = 5, QUAD_PIXELS = 4 };

typedef struct quad_candidate {
    vec2 vector;
    int mode;
    float coverage[QUAD_PIXELS];
    float cost[QUAD_PIXELS];
    unsigned sides[QUAD_PIXELS];
    float weight_a[QUAD_PIXELS];
    float weight_b[QUAD_PIXELS];
} quad_candidate;

typedef struct quad_values {
    float v[QUAD_PIXELS];
} quad_values;

typedef struct quad_state {
    const mb_interpolate_pass *pass;
    ivec2 q0;
    float t;
    float scale;
    vec2 inv;
    quad_candidate candidates[QUAD_MAX_CANDIDATES];
    int count;
    int fallback_index;
    bool has_uncover;
    float fallback_sum[QUAD_PIXELS];
    float panel_sum[QUAD_PIXELS];
    float coverage_sum[QUAD_PIXELS];
    vec4 offset;
    float luma_offset;
    float panel_share;
    float panel_switch;
    float global_a[QUAD_PIXELS];
    float global_b[QUAD_PIXELS];
    vec2 global_chroma_a;
    vec2 global_chroma_b;
} quad_state;

typedef struct covered_pixels {
    int index[QUAD_PIXELS];
    int count;
} covered_pixels;

covered_pixels quad_covered_pixels(const mb_chroma_layout *chroma, ivec2 chroma_index);
void quad_begin(quad_state *q, const mb_interpolate_pass *pass, ivec2 q0);
void quad_gather_blocks(quad_state *q);
void quad_add_fallback(quad_state *q);
void quad_score(quad_state *q);
void quad_weigh(quad_state *q);
quad_values quad_blend_luma(const quad_state *q);
void quad_measure_panels(quad_state *q);
quad_values quad_mix_panels_luma(const quad_state *q, quad_values luma);
vec2 quad_blend_chroma(const quad_state *q, ivec2 chroma_index, ivec2 chroma_position);
float quad_output_value(const mb_output_range *range, float value);

#endif
