#ifndef MOTIONBLADE_RESOURCES_H
#define MOTIONBLADE_RESOURCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mb_image mb_image;
typedef struct mb_buffer mb_buffer;

typedef enum mb_texel_format {
    MB_TEXELS_R8,
    MB_TEXELS_RG8,
    MB_TEXELS_R16,
    MB_TEXELS_RG16,
    MB_TEXELS_R16F,
} mb_texel_format;

typedef struct mb_image_desc {
    int width;
    int height;
    mb_texel_format format;
} mb_image_desc;

typedef struct mb_grid {
    int blocks_x;
    int blocks_y;
    int width;
    int height;
} mb_grid;

typedef struct mb_chroma_layout {
    int width;
    int height;
    int shift_x;
    int shift_y;
    bool interleaved;
} mb_chroma_layout;

typedef struct mb_frame_planes {
    mb_image *luma;
    mb_image *chroma[2];
} mb_frame_planes;

int mb_grid_block_count(mb_grid grid);
int mb_texel_channels(mb_texel_format format);
int mb_texel_sample_bytes(mb_texel_format format);
size_t mb_texel_count(mb_image_desc desc);
mb_image *mb_frame_plane(mb_frame_planes planes, int plane);

#endif
