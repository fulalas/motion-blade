#ifndef MOTIONBLADE_SCHEDULE_H
#define MOTIONBLADE_SCHEDULE_H

#include <stdbool.h>

enum { MB_FACTOR_AUTO = 0, MB_FACTOR_OFF = -1, MB_MAX_FACTOR = 8 };

typedef struct mb_schedule_config {
    int factor;
    double max_fps;
    double max_gap;
    int max_pending;
} mb_schedule_config;

typedef struct mb_timed_frame {
    void *frame;
    double pts;
} mb_timed_frame;

typedef enum mb_action_kind {
    MB_ACTION_PASS,
    MB_ACTION_DRAIN,
    MB_ACTION_RESET_ENGINE,
    MB_ACTION_PUSH,
    MB_ACTION_END_STREAM,
    MB_ACTION_EMIT_REAL,
    MB_ACTION_EMIT_INTERPOLATED,
    MB_ACTION_RELEASE,
} mb_action_kind;

typedef struct mb_action {
    mb_action_kind kind;
    void *frame;
    double pts;
    double duration;
    double t;
} mb_action;

typedef struct mb_action_sink {
    bool (*run)(void *context, mb_action *action);
    void *context;
} mb_action_sink;

typedef struct mb_schedule mb_schedule;

mb_schedule *mb_schedule_create(const mb_schedule_config *config, mb_action_sink sink);
void mb_schedule_destroy(mb_schedule *schedule);
void mb_schedule_set_factor(mb_schedule *schedule, int factor);
void mb_schedule_set_capacity(mb_schedule *schedule, int max_pending);
int mb_schedule_factor(const mb_schedule *schedule);
void mb_schedule_frame(mb_schedule *schedule, mb_timed_frame frame, double display_fps);
void mb_schedule_end(mb_schedule *schedule, double display_fps);
void mb_schedule_reset(mb_schedule *schedule);
void mb_schedule_forget_frames(mb_schedule *schedule);
void mb_schedule_stop(mb_schedule *schedule);

#endif
