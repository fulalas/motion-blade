#ifndef MB_LAYER_COMMON_H
#define MB_LAYER_COMMON_H

#include "common/vec.h"
#include "motionblade/layout.h"
#include "motionblade/passes.h"

typedef struct layer_kernel {
    float weights[2 * MB_LAYER_SMOOTH_RADIUS + 1];
    int radius;
} layer_kernel;

int layer_cell_index(mb_layer_grid grid, int x, int y);
vec4 layer_field_at(const vec4 *field, mb_layer_grid grid, vec2 top_position);
vec2 layer_mask_at(const vec2 *mask, int width, int height, vec2 index_position);
layer_kernel layer_gauss(float sigma, int radius);
bool layer_is_cut(const mb_buffer *stats);
bool layer_is_active(const mb_buffer *model);
bool layer_any_active(mb_buffer *const models[2]);
bool layer_gate_open(mb_buffer *const models[2], const mb_buffer *trigger);

#endif
