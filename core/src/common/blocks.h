#ifndef MB_BLOCKS_H
#define MB_BLOCKS_H

#include "common/vec.h"
#include "motionblade/layout.h"
#include "motionblade/resources.h"

static const ivec2 RING8[8] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

static inline int block_index(ivec2 block, mb_grid grid)
{
    int x = clampi(block.x, 0, grid.blocks_x - 1);
    int y = clampi(block.y, 0, grid.blocks_y - 1);
    return y * grid.blocks_x + x;
}

static inline ivec2 block_coord(int index, mb_grid grid)
{
    return iv2(index % grid.blocks_x, index / grid.blocks_x);
}

static inline bool block_inside(ivec2 block, mb_grid grid)
{
    return block.x >= 0 && block.y >= 0 && block.x < grid.blocks_x && block.y < grid.blocks_y;
}

static inline ivec2 neighbour3(int k)
{
    return iv2(k % 3 - 1, k / 3 - 1);
}

static inline int neighbour3_index(int k, ivec2 block, mb_grid grid)
{
    return block_index(iv2_add(block, neighbour3(k)), grid);
}

static inline vec2 block_center(ivec2 block)
{
    int half = MB_BLOCK_SIZE / 2;
    return v2((float)(block.x * MB_BLOCK_SIZE + half), (float)(block.y * MB_BLOCK_SIZE + half));
}

typedef struct block_taps {
    int index[4];
    float weight[4];
} block_taps;

static inline block_taps block_taps_at(vec2 level_position, mb_grid grid)
{
    const float center = MB_BLOCK_SIZE * 0.5f - 0.5f;
    vec2 g = v2((level_position.x - center) / MB_BLOCK_SIZE, (level_position.y - center) / MB_BLOCK_SIZE);
    ivec2 b0 = iv2_floor(g);
    vec2 fr = v2(g.x - (float)b0.x, g.y - (float)b0.y);
    block_taps taps;
    for (int k = 0; k < 4; k++) {
        taps.index[k] = block_index(iv2(b0.x + (k & 1), b0.y + (k >> 1)), grid);
        taps.weight[k] = ((k & 1) ? fr.x : 1.0f - fr.x) * ((k >> 1) ? fr.y : 1.0f - fr.y);
    }
    return taps;
}

static inline ivec2 window_origin(ivec2 block)
{
    return iv2(block.x * MB_BLOCK_SIZE - MB_WINDOW_MARGIN, block.y * MB_BLOCK_SIZE - MB_WINDOW_MARGIN);
}

static inline ivec2 block_at(vec2 position)
{
    return iv2_floor(v2_scale(position, 1.0f / MB_BLOCK_SIZE));
}

static inline float consistency_limit(float threshold, vec2 u)
{
    return threshold * 0.25f + v2_l1(u) / 8.0f;
}

#endif
