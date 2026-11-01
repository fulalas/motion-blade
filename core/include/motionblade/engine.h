#ifndef MOTIONBLADE_ENGINE_H
#define MOTIONBLADE_ENGINE_H

#include <stdbool.h>

#include "motionblade/backend.h"
#include "motionblade/log.h"
#include "motionblade/params.h"
#include "motionblade/status.h"
#include "motionblade/video.h"

typedef struct mb_engine mb_engine;

typedef struct mb_engine_config {
    mb_video_format format;
    mb_backend backend;
    mb_logger logger;
    mb_params params;
} mb_engine_config;

typedef struct mb_frame_report {
    bool valid;
    bool scene_cut;
    double mean_error;
    double gpu_ms;
} mb_frame_report;

typedef enum mb_field_kind {
    MB_FIELD_FORWARD,
    MB_FIELD_BACKWARD,
    MB_FIELD_SELECTION,
} mb_field_kind;

typedef struct mb_field_dump {
    float *vectors;
    int blocks_x;
    int blocks_y;
} mb_field_dump;

mb_status mb_engine_create(const mb_engine_config *config, mb_engine **engine);
void mb_engine_destroy(mb_engine *engine);
bool mb_engine_is_healthy(const mb_engine *engine);
bool mb_engine_uses_lookahead(const mb_engine *engine);
mb_status mb_engine_set_param(mb_engine *engine, const char *name, double value);

mb_status mb_engine_push(mb_engine *engine, const mb_host_image *frame);
mb_status mb_engine_push_device(mb_engine *engine, const mb_device_frame *frame);
void mb_engine_end_stream(mb_engine *engine);
void mb_engine_reset(mb_engine *engine);

mb_status mb_engine_submit(mb_engine *engine, double t);
mb_status mb_engine_submit_device(mb_engine *engine, double t, const mb_device_frame *target);
int mb_engine_pending(const mb_engine *engine);
bool mb_engine_output_ready(const mb_engine *engine);
mb_status mb_engine_retrieve(mb_engine *engine, const mb_host_image *target, mb_frame_report *report);
mb_status mb_engine_finish_device(mb_engine *engine, mb_frame_report *report);

mb_status mb_engine_read_field(mb_engine *engine, mb_field_kind kind, mb_field_dump *dump);

#endif
