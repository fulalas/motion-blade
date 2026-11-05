#ifndef MOTIONBLADE_PULLDOWN_H
#define MOTIONBLADE_PULLDOWN_H

#include <stdbool.h>
#include <stddef.h>

#include "motionblade/video.h"

typedef enum mb_pulldown {
    MB_PULLDOWN_NONE,
    MB_PULLDOWN_TOP_FIRST,
    MB_PULLDOWN_BOTTOM_FIRST,
} mb_pulldown;

typedef struct mb_field_motion {
    double top;
    double bottom;
} mb_field_motion;

typedef struct mb_frame_pair {
    const mb_video_format *format;
    const mb_host_image *previous;
    const mb_host_image *current;
} mb_frame_pair;

mb_field_motion mb_field_motion_measure(mb_frame_pair pair);
double mb_frame_motion(mb_frame_pair pair);
bool mb_frame_repeats(double motion_before, double motion, double motion_after);

typedef enum mb_repeat_decision {
    MB_REPEAT_WAIT,
    MB_REPEAT_SHOW_PENDING,
    MB_REPEAT_DROP_PENDING,
} mb_repeat_decision;

typedef enum mb_motion_origin {
    MB_FROM_SHOWN,
    MB_FROM_PENDING,
} mb_motion_origin;

typedef struct mb_motion_source {
    double (*motion_to_new)(void *context, mb_motion_origin origin);
    void *context;
} mb_motion_source;

typedef struct mb_repeat_filter {
    bool has_pending;
    double motion_shown;
    double motion_pending;
} mb_repeat_filter;

void mb_repeat_filter_reset(mb_repeat_filter *filter);
mb_repeat_decision mb_repeat_filter_push(mb_repeat_filter *filter, mb_motion_source source);

typedef struct mb_pulldown_detector mb_pulldown_detector;

mb_pulldown_detector *mb_pulldown_create(void);
void mb_pulldown_destroy(mb_pulldown_detector *detector);
void mb_pulldown_forget(mb_pulldown_detector *detector);
mb_pulldown mb_pulldown_push(mb_pulldown_detector *detector, mb_field_motion motion);
mb_pulldown mb_pulldown_state(const mb_pulldown_detector *detector);

#endif
