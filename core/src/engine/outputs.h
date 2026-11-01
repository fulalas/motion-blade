#ifndef MB_OUTPUTS_H
#define MB_OUTPUTS_H

#include "engine/engine_state.h"

mb_status outputs_submit(mb_engine *engine, output_kind kind);
int outputs_pending(const mb_engine *engine);
bool outputs_head_ready(const mb_engine *engine);
mb_status outputs_retrieve_host(mb_engine *engine, const mb_host_image *target, mb_frame_report *report);
mb_status outputs_finish_device(mb_engine *engine, mb_frame_report *report);
void outputs_discard_all(mb_engine *engine);

#endif
