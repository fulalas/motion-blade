#include "motionblade/resources.h"

int mb_grid_block_count(mb_grid grid)
{
    return grid.blocks_x * grid.blocks_y;
}

int mb_texel_channels(mb_texel_format format)
{
    return format == MB_TEXELS_RG8 || format == MB_TEXELS_RG16 ? 2 : 1;
}

int mb_texel_sample_bytes(mb_texel_format format)
{
    return format == MB_TEXELS_R8 || format == MB_TEXELS_RG8 ? 1 : 2;
}

size_t mb_texel_count(mb_image_desc desc)
{
    return (size_t)desc.width * (size_t)desc.height * (size_t)mb_texel_channels(desc.format);
}

mb_image *mb_frame_plane(mb_frame_planes planes, int plane)
{
    return plane == 0 ? planes.luma : planes.chroma[plane - 1];
}
