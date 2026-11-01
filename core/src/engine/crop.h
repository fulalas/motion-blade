#ifndef MB_CROP_H
#define MB_CROP_H

#include "engine/engine_state.h"

mb_status crop_to(mb_engine *engine, mb_rect active);
mb_status allocate_geometry(mb_engine *engine);

#endif
