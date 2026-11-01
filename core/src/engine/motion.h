#ifndef MB_MOTION_H
#define MB_MOTION_H

#include "engine/engine_state.h"

int ring_index(const mb_engine *engine, int back);
mb_image *level_image(const mb_engine *engine, int slot, int level);
void build_levels(mb_engine *engine, int slot);
void rotate_fields(mb_engine *engine);
mb_status run_motion(mb_engine *engine);

#endif
