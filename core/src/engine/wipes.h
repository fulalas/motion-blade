#ifndef MB_ENGINE_WIPES_H
#define MB_ENGINE_WIPES_H

#include "engine/engine_state.h"
#include "engine/interpolation.h"

size_t wipe_cells(const geometry *g);
void estimate_wipes(mb_engine *engine);
void composite_wipes(mb_engine *engine, const pair_choice *pair, mb_warp warp);

#endif
