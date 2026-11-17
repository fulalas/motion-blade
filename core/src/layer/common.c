#include "layer/common.h"

#include "reference/cpu_resources.h"

int layer_cell_index(mb_layer_grid grid, int x, int y)
{
    return clampi(y, 0, grid.cells_y - 1) * grid.cells_x + clampi(x, 0, grid.cells_x - 1);
}

static vec4 mix4(vec4 a, vec4 b, float w)
{
    return (vec4){mixf(a.x, b.x, w), mixf(a.y, b.y, w), mixf(a.z, b.z, w), mixf(a.w, b.w, w)};
}

vec4 layer_field_at(const vec4 *field, mb_layer_grid grid, vec2 top_position)
{
    float half = 0.5f * (float)MB_LAYER_CELL;
    float gx = clampf((top_position.x - half) / (float)MB_LAYER_CELL, 0.0f, (float)(grid.cells_x - 1));
    float gy = clampf((top_position.y - half) / (float)MB_LAYER_CELL, 0.0f, (float)(grid.cells_y - 1));
    int x0 = (int)floorf(gx);
    int y0 = (int)floorf(gy);
    float fx = gx - (float)x0;
    float fy = gy - (float)y0;
    vec4 top = mix4(field[layer_cell_index(grid, x0, y0)], field[layer_cell_index(grid, x0 + 1, y0)], fx);
    vec4 bottom = mix4(field[layer_cell_index(grid, x0, y0 + 1)], field[layer_cell_index(grid, x0 + 1, y0 + 1)], fx);
    return mix4(top, bottom, fy);
}

static vec2 mask_value(const vec2 *mask, int width, int height, int x, int y)
{
    return mask[clampi(y, 0, height - 1) * width + clampi(x, 0, width - 1)];
}

static vec2 mix2(vec2 a, vec2 b, float w)
{
    return v2(mixf(a.x, b.x, w), mixf(a.y, b.y, w));
}

vec2 layer_mask_at(const vec2 *mask, int width, int height, vec2 index_position)
{
    float gx = clampf(index_position.x, 0.0f, (float)(width - 1));
    float gy = clampf(index_position.y, 0.0f, (float)(height - 1));
    int x0 = (int)floorf(gx);
    int y0 = (int)floorf(gy);
    float fx = gx - (float)x0;
    float fy = gy - (float)y0;
    vec2 top = mix2(mask_value(mask, width, height, x0, y0), mask_value(mask, width, height, x0 + 1, y0), fx);
    vec2 bottom = mix2(mask_value(mask, width, height, x0, y0 + 1), mask_value(mask, width, height, x0 + 1, y0 + 1), fx);
    return mix2(top, bottom, fy);
}

layer_kernel layer_gauss(float sigma, int radius)
{
    layer_kernel kernel = {.radius = radius};
    float sum = 0.0f;
    for (int i = -radius; i <= radius; i++) {
        kernel.weights[i + radius] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
        sum += kernel.weights[i + radius];
    }
    for (int i = 0; i <= 2 * radius; i++) {
        kernel.weights[i] /= sum;
    }
    return kernel;
}

bool layer_is_cut(const mb_buffer *stats)
{
    return stats && words(stats)[MB_STAT_CUT] != 0u;
}

bool layer_is_active(const mb_buffer *model)
{
    return model && vec4s(model)[0].z > 0.5f;
}

bool layer_any_active(mb_buffer *const models[2])
{
    return layer_is_active(models[0]) || layer_is_active(models[1]);
}

bool layer_gate_open(mb_buffer *const models[2], const mb_buffer *trigger)
{
    return layer_any_active(models) && (!trigger || vec4s(trigger)[0].z > (float)MB_LAYER_ADOPTED);
}
