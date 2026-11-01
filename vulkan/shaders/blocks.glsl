#include "motionblade/layout.h"

const ivec2 RING8[8] = ivec2[8](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
                                ivec2(-1, -1), ivec2(1, -1), ivec2(-1, 1), ivec2(1, 1));

float l1(vec2 a)
{
    return abs(a.x) + abs(a.y);
}

float consistency_limit(float threshold, vec2 u)
{
    return threshold * 0.25 + l1(u) / 8.0;
}

ivec2 blocks()
{
    return ivec2(push.blocks_x, push.blocks_y);
}

int block_index(ivec2 block, ivec2 blocks)
{
    block = clamp(block, ivec2(0), blocks - 1);
    return block.y * blocks.x + block.x;
}

ivec2 neighbour3(int k)
{
    return ivec2(k % 3 - 1, k / 3 - 1);
}

int neighbour3_index(int k, ivec2 block, ivec2 blocks)
{
    return block_index(block + neighbour3(k), blocks);
}

ivec2 block_of(int id, int blocks_x)
{
    return ivec2(id % blocks_x, id / blocks_x);
}

vec2 block_center(ivec2 block)
{
    return vec2(block * MB_BLOCK_SIZE + MB_BLOCK_SIZE / 2);
}

ivec2 window_origin(ivec2 block)
{
    return block * MB_BLOCK_SIZE - MB_WINDOW_MARGIN;
}

ivec2 block_at(vec2 position)
{
    return ivec2(floor(position / float(MB_BLOCK_SIZE)));
}
