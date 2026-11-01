#ifndef MB_BAR_TRACKER_H
#define MB_BAR_TRACKER_H

#include "framing/bars.h"

typedef struct bar_tracker {
    bool seen;
    int scan;
    int pushes_seen;
    int stable_pushes;
    mb_rect candidate;
} bar_tracker;

typedef struct bar_decision {
    bool crop;
    mb_rect active;
} bar_decision;

typedef struct bar_observation {
    const bar_source *source;
    mb_rect active;
    int push_count;
    int settle_pushes;
} bar_observation;

bar_decision bar_tracker_update(bar_tracker *tracker, const bar_observation *observation);

#endif
