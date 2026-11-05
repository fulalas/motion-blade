#ifndef MB_GEOMETRY_H
#define MB_GEOMETRY_H

#include "engine/engine_state.h"

geometry geometry_for(const mb_video_format *source, mb_rect active, const mb_search_params *search);
mb_grid geometry_base_grid(const geometry *g);
int geometry_base_blocks(const geometry *g);
double stat_error_mean(double sum, int blocks);
bool geometry_is_cropped(const geometry *g, const mb_video_format *source);
float geometry_luma_scale(const geometry *g);
mb_output_range geometry_output_range(const geometry *g);
float geometry_chroma_mid(const geometry *g);
mb_texel_format geometry_luma_texels(const geometry *g);
mb_texel_format geometry_chroma_texels(const geometry *g);

#endif
