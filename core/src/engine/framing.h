#ifndef MB_FRAMING_H
#define MB_FRAMING_H

#include "engine/engine_state.h"

mb_status framing_observe_host(mb_engine *engine, const mb_host_image *frame);
mb_status framing_observe_device(mb_engine *engine);
void framing_save_border(mb_engine *engine, const mb_host_image *frame);
mb_device_load framing_plan_device_load(mb_engine *engine, const mb_device_frame *frame, mb_frame_planes planes);
void framing_note_device_load(mb_engine *engine, const mb_device_load *load);
mb_border framing_output_border(const mb_engine *engine);

#endif
