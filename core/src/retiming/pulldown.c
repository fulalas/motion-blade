#include "motionblade/pulldown.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum { CYCLE = 5, WINDOW = 4 * CYCLE, RECENT = 2 * CYCLE, COLUMN_STEP = 4 };

static const double MOTION_FLOOR = 2.0;
static const double REPEAT_RATIO = 0.5;
static const double EVEN_RATIO = 0.9;
static const double REPEATED_FRAME_RATIO = 0.4;
static const int LOCK_HITS = 6;
static const int LOCK_MISSES = 2;
static const int QUICK_LOCK_HITS = 4;
static const int UNLOCK_MOVING = RECENT - 2;

typedef enum field_class {
    FIELDS_STILL,
    FIELDS_MOVING,
    FIELDS_UNEVEN,
    FIELDS_REPEAT_TOP,
    FIELDS_REPEAT_BOTTOM,
} field_class;

typedef struct cadence_score {
    int hits;
    int misses;
} cadence_score;

struct mb_pulldown_detector {
    field_class history[WINDOW];
    long long pushed;
    mb_pulldown state;
};

static int sample_at(const mb_luma_plane *plane, int x, int y)
{
    const uint8_t *row = (const uint8_t *)plane->data + y * plane->stride;
    return plane->bytes_per_sample == 1 ? row[x] : ((const uint16_t *)row)[x];
}

static double field_difference(const mb_luma_plane *a, const mb_luma_plane *b, int parity)
{
    double sum = 0.0;
    long long count = 0;
    for (int y = parity; y < b->height; y += 2) {
        for (int x = 0; x < b->width; x += COLUMN_STEP) {
            sum += abs(sample_at(a, x, y) - sample_at(b, x, y));
            count++;
        }
    }
    return count ? sum * 255.0 / ((double)b->max_value * count) : 0.0;
}

mb_field_motion mb_field_motion_measure(const mb_luma_plane *previous, const mb_luma_plane *current)
{
    return (mb_field_motion){field_difference(previous, current, 0), field_difference(previous, current, 1)};
}

double mb_frame_motion(const mb_luma_plane *previous, const mb_luma_plane *current)
{
    mb_field_motion m = mb_field_motion_measure(previous, current);
    return (m.top + m.bottom) / 2.0;
}

bool mb_frame_repeats(double motion_before, double motion, double motion_after)
{
    double neighbours = isnan(motion_before) || motion_after < motion_before ? motion_after : motion_before;
    return neighbours >= MOTION_FLOOR && motion < REPEATED_FRAME_RATIO * neighbours;
}

mb_pulldown_detector *mb_pulldown_create(void)
{
    return calloc(1, sizeof(mb_pulldown_detector));
}

void mb_pulldown_destroy(mb_pulldown_detector *detector)
{
    free(detector);
}

void mb_pulldown_forget(mb_pulldown_detector *detector)
{
    detector->pushed = 0;
}

mb_pulldown mb_pulldown_state(const mb_pulldown_detector *detector)
{
    return detector->state;
}

static field_class classify(mb_field_motion m)
{
    double high = m.top > m.bottom ? m.top : m.bottom;
    double low = m.top > m.bottom ? m.bottom : m.top;
    if (high < MOTION_FLOOR) {
        return FIELDS_STILL;
    }
    if (low >= EVEN_RATIO * high) {
        return FIELDS_MOVING;
    }
    if (low >= REPEAT_RATIO * high) {
        return FIELDS_UNEVEN;
    }
    return m.top < m.bottom ? FIELDS_REPEAT_TOP : FIELDS_REPEAT_BOTTOM;
}

static field_class expected_class(mb_pulldown parity, int offset)
{
    field_class first = parity == MB_PULLDOWN_TOP_FIRST ? FIELDS_REPEAT_TOP : FIELDS_REPEAT_BOTTOM;
    field_class second = parity == MB_PULLDOWN_TOP_FIRST ? FIELDS_REPEAT_BOTTOM : FIELDS_REPEAT_TOP;
    return offset == 0 ? first : offset == 2 ? second : FIELDS_MOVING;
}

static field_class recent_class(const mb_pulldown_detector *d, long long n)
{
    return d->history[n % WINDOW];
}

static cadence_score score_phase(const mb_pulldown_detector *d, mb_pulldown parity, int phase, int span)
{
    cadence_score score = {0, 0};
    for (long long n = d->pushed - span; n < d->pushed; n++) {
        field_class seen = recent_class(d, n) == FIELDS_UNEVEN ? FIELDS_MOVING : recent_class(d, n);
        field_class expected = expected_class(parity, (int)((n + CYCLE - phase) % CYCLE));
        if (seen == FIELDS_STILL) {
            continue;
        }
        if (seen != expected) {
            score.misses++;
        } else if (expected != FIELDS_MOVING) {
            score.hits++;
        }
    }
    return score;
}

static cadence_score best_score(const mb_pulldown_detector *d, mb_pulldown parity, int span)
{
    cadence_score best = {0, span};
    for (int phase = 0; phase < CYCLE; phase++) {
        cadence_score s = score_phase(d, parity, phase, span);
        if (s.hits - s.misses > best.hits - best.misses) {
            best = s;
        }
    }
    return best;
}

static bool locks(const mb_pulldown_detector *d, mb_pulldown parity)
{
    cadence_score recent = best_score(d, parity, RECENT);
    if (recent.hits >= QUICK_LOCK_HITS && recent.misses == 0) {
        return true;
    }
    cadence_score window = d->pushed >= WINDOW ? best_score(d, parity, WINDOW) : (cadence_score){0, 0};
    return window.hits >= LOCK_HITS && window.misses <= LOCK_MISSES;
}

static bool against_film(const mb_pulldown_detector *d)
{
    int moving = 0;
    int repeats = 0;
    for (long long n = d->pushed - RECENT; n < d->pushed; n++) {
        field_class c = recent_class(d, n);
        moving += c != FIELDS_STILL;
        repeats += c != FIELDS_STILL && c != FIELDS_MOVING;
    }
    return moving >= UNLOCK_MOVING && repeats == 0;
}

static mb_pulldown other_parity(mb_pulldown parity)
{
    return parity == MB_PULLDOWN_TOP_FIRST ? MB_PULLDOWN_BOTTOM_FIRST : MB_PULLDOWN_TOP_FIRST;
}

static mb_pulldown decide(const mb_pulldown_detector *d)
{
    if (d->state == MB_PULLDOWN_NONE) {
        return locks(d, MB_PULLDOWN_TOP_FIRST)      ? MB_PULLDOWN_TOP_FIRST
               : locks(d, MB_PULLDOWN_BOTTOM_FIRST) ? MB_PULLDOWN_BOTTOM_FIRST
                                                    : MB_PULLDOWN_NONE;
    }
    if (locks(d, other_parity(d->state)) && !locks(d, d->state)) {
        return other_parity(d->state);
    }
    return against_film(d) ? MB_PULLDOWN_NONE : d->state;
}

mb_pulldown mb_pulldown_push(mb_pulldown_detector *d, mb_field_motion motion)
{
    d->history[d->pushed % WINDOW] = classify(motion);
    d->pushed++;
    if (d->pushed >= RECENT) {
        d->state = decide(d);
    }
    return d->state;
}
