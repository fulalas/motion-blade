#ifndef MOTIONBLADE_PULLDOWN_H
#define MOTIONBLADE_PULLDOWN_H

#include <stdbool.h>
#include <stddef.h>

typedef enum mb_pulldown {
    MB_PULLDOWN_NONE,
    MB_PULLDOWN_TOP_FIRST,
    MB_PULLDOWN_BOTTOM_FIRST,
} mb_pulldown;

typedef struct mb_luma_plane {
    const void *data;
    ptrdiff_t stride;
    int width;
    int height;
    int bytes_per_sample;
    int max_value;
} mb_luma_plane;

typedef struct mb_field_motion {
    double top;
    double bottom;
} mb_field_motion;

mb_field_motion mb_field_motion_measure(const mb_luma_plane *previous, const mb_luma_plane *current);
double mb_frame_motion(const mb_luma_plane *previous, const mb_luma_plane *current);
bool mb_frame_repeats(double motion_before, double motion, double motion_after);

typedef struct mb_pulldown_detector mb_pulldown_detector;

mb_pulldown_detector *mb_pulldown_create(void);
void mb_pulldown_destroy(mb_pulldown_detector *detector);
void mb_pulldown_forget(mb_pulldown_detector *detector);
mb_pulldown mb_pulldown_push(mb_pulldown_detector *detector, mb_field_motion motion);
mb_pulldown mb_pulldown_state(const mb_pulldown_detector *detector);

#endif
