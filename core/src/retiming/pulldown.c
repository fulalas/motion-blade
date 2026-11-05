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

static const void *row_of(const mb_host_image *image, int y)
{
    return image->planes[0] + y * image->strides[0];
}

static long long row_difference_8(const uint8_t *a, const uint8_t *b, int width)
{
    long long sum = 0;
    for (int x = 0; x < width; x += COLUMN_STEP) {
        sum += abs(a[x] - b[x]);
    }
    return sum;
}

static long long row_difference_16(const uint16_t *a, const uint16_t *b, int width)
{
    long long sum = 0;
    for (int x = 0; x < width; x += COLUMN_STEP) {
        sum += abs(a[x] - b[x]);
    }
    return sum;
}

static long long row_difference(mb_frame_pair pair, int y)
{
    int width = pair.format->width;
    if (mb_bytes_per_sample(pair.format) == 1) {
        return row_difference_8(row_of(pair.previous, y), row_of(pair.current, y), width);
    }
    return row_difference_16(row_of(pair.previous, y), row_of(pair.current, y), width);
}

static double field_difference(mb_frame_pair pair, int parity)
{
    const mb_video_format *f = pair.format;
    long long sum = 0;
    long long rows = 0;
    for (int y = parity; y < f->height; y += 2) {
        sum += row_difference(pair, y);
        rows++;
    }
    long long count = rows * ((f->width + COLUMN_STEP - 1) / COLUMN_STEP);
    double max_value = (double)((1 << f->bit_depth) - 1);
    return count ? (double)sum * 255.0 / (max_value * count) : 0.0;
}

mb_field_motion mb_field_motion_measure(mb_frame_pair pair)
{
    return (mb_field_motion){field_difference(pair, 0), field_difference(pair, 1)};
}

double mb_frame_motion(mb_frame_pair pair)
{
    mb_field_motion m = mb_field_motion_measure(pair);
    return (m.top + m.bottom) / 2.0;
}

bool mb_frame_repeats(double motion_before, double motion, double motion_after)
{
    double neighbours = isnan(motion_before) || motion_after < motion_before ? motion_after : motion_before;
    return neighbours >= MOTION_FLOOR && motion < REPEATED_FRAME_RATIO * neighbours;
}

void mb_repeat_filter_reset(mb_repeat_filter *filter)
{
    *filter = (mb_repeat_filter){false, NAN, NAN};
}

static double motion_from(mb_motion_source source, mb_motion_origin origin)
{
    return source.motion_to_new(source.context, origin);
}

mb_repeat_decision mb_repeat_filter_push(mb_repeat_filter *filter, mb_motion_source source)
{
    if (!filter->has_pending) {
        filter->has_pending = true;
        filter->motion_pending = motion_from(source, MB_FROM_SHOWN);
        return MB_REPEAT_WAIT;
    }
    double motion_after = motion_from(source, MB_FROM_PENDING);
    if (!mb_frame_repeats(filter->motion_shown, filter->motion_pending, motion_after)) {
        filter->motion_shown = filter->motion_pending;
        filter->motion_pending = motion_after;
        return MB_REPEAT_SHOW_PENDING;
    }
    filter->motion_pending = motion_from(source, MB_FROM_SHOWN);
    return MB_REPEAT_DROP_PENDING;
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
    if (d->pushed < RECENT) {
        return false;
    }
    cadence_score recent = best_score(d, parity, RECENT);
    if (recent.hits >= QUICK_LOCK_HITS && recent.misses == 0) {
        return true;
    }
    cadence_score window = d->pushed >= WINDOW ? best_score(d, parity, WINDOW) : (cadence_score){0, 0};
    return window.hits >= LOCK_HITS && window.misses <= LOCK_MISSES;
}

static bool against_film(const mb_pulldown_detector *d)
{
    int span = d->pushed < RECENT ? (int)d->pushed : RECENT;
    int moving = 0;
    int repeats = 0;
    for (long long n = d->pushed - span; n < d->pushed; n++) {
        field_class c = recent_class(d, n);
        moving += c != FIELDS_STILL;
        repeats += c != FIELDS_STILL && c != FIELDS_MOVING;
    }
    int needed = span == RECENT ? UNLOCK_MOVING : span;
    return moving >= needed && repeats == 0;
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
    if (d->pushed >= CYCLE) {
        d->state = decide(d);
    }
    return d->state;
}
