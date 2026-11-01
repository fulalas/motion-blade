#ifndef MB_RESOURCES_H
#define MB_RESOURCES_H

#include "engine/engine_state.h"

mb_status frames_create(mb_engine *engine);
void frames_destroy(const mb_backend *backend, frame_slot frames[FRAME_RING]);
mb_status buffers_create(mb_engine *engine);
void buffers_destroy(mb_engine *engine);

#endif
