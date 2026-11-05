#ifndef MB_MOTION_H
#define MB_MOTION_H

#include "engine/engine_state.h"

mb_mean_removal mean_removal_of(const mb_search_params *s);
mb_support_tolerance support_tolerance_of(const mb_select_params *s);
int ring_index(const mb_engine *engine, int back);
pair_motion *pair_at(const mb_engine *engine, int back);
void motion_advance(mb_engine *engine);
void motion_forget(mb_engine *engine);
mb_image *level_image(const mb_engine *engine, int slot, int level);
void build_levels(mb_engine *engine, int slot);
mb_status run_motion(mb_engine *engine);

#endif
