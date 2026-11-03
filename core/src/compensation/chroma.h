#ifndef MB_CHROMA_H
#define MB_CHROMA_H

#include "common/texture.h"
#include "common/vec.h"
#include "motionblade/resources.h"

typedef struct chroma_channels {
    ref_texture *first;
    ref_texture *second;
    int second_channel;
} chroma_channels;

chroma_channels chroma_channels_of(const mb_frame_planes *planes, const mb_chroma_layout *chroma);
vec2 chroma_from_luma(const mb_chroma_layout *chroma, vec2 luma);
vec2 chroma_sample(const mb_frame_planes *planes, const mb_chroma_layout *chroma, vec2 position);
vec2 chroma_cubic_sample(const mb_frame_planes *planes, const mb_chroma_layout *chroma, vec2 position);

#endif
