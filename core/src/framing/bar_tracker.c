#include "framing/bar_tracker.h"

enum { SCAN_CYCLE = 12, RECHECK_MASK = 3 };

static const bar_decision KEEP = {false, {0, 0, 0, 0}};

static bool skip_host_scan(bar_tracker *tracker, const bar_observation *o)
{
    if (o->source->flags || !tracker->seen || tracker->scan == 0) {
        return false;
    }
    return (tracker->scan & RECHECK_MASK) || bars_still_black(o->source, o->active);
}

static bar_decision first_sight(bar_tracker *tracker, const bar_observation *o, mb_rect found)
{
    tracker->seen = true;
    return mb_rect_equal(found, o->active) ? KEEP : (bar_decision){true, found};
}

static bar_decision settle(bar_tracker *tracker, const bar_observation *o, mb_rect found, int pushes)
{
    if (!mb_rect_equal(found, tracker->candidate)) {
        tracker->candidate = found;
        tracker->stable_pushes = 0;
    }
    tracker->stable_pushes += pushes;
    if (tracker->stable_pushes < o->settle_pushes) {
        return KEEP;
    }
    tracker->stable_pushes = 0;
    return (bar_decision){true, found};
}

bar_decision bar_tracker_update(bar_tracker *tracker, const bar_observation *o)
{
    tracker->scan = (tracker->scan + 1) % SCAN_CYCLE;
    if (skip_host_scan(tracker, o)) {
        return KEEP;
    }
    int pushes = o->push_count - tracker->pushes_seen;
    tracker->pushes_seen = o->push_count;
    mb_rect found;
    if (!bars_detect(o->source, &found)) {
        return KEEP;
    }
    if (!tracker->seen) {
        return first_sight(tracker, o, found);
    }
    if (!mb_rect_contains(o->active, found)) {
        tracker->stable_pushes = 0;
        return (bar_decision){true, mb_rect_union(found, o->active)};
    }
    if (mb_rect_equal(found, o->active)) {
        tracker->stable_pushes = 0;
        return KEEP;
    }
    return settle(tracker, o, found, pushes);
}
