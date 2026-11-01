#include "motionblade/resources.h"

int mb_grid_block_count(mb_grid grid)
{
    return grid.blocks_x * grid.blocks_y;
}

int mb_texel_channels(mb_texel_format format)
{
    return format == MB_TEXELS_RG8 || format == MB_TEXELS_RG16 ? 2 : 1;
}
