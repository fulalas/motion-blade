#ifndef MB_ENGINE_LAYERS_H
#define MB_ENGINE_LAYERS_H

#include "engine/engine_state.h"
#include "engine/interpolation.h"

size_t layer_cells(const geometry *g);
size_t layer_mask_texels(const geometry *g);
void estimate_layers(mb_engine *engine);
void composite_layers(mb_engine *engine, const pair_choice *pair, mb_warp warp);

#endif
