#include "motionblade/schedule.h"

#include <math.h>
#include <stdlib.h>

static const double RATE_TOLERANCE = 1.05;
static const double REAL_FRAME_T = 0.99;
static const double GRID_SLACK = 0.01;

typedef struct held_frame {
    void *frame;
    double pts;
    bool present;
} held_frame;

struct mb_schedule {
    mb_schedule_config config;
    mb_action_sink sink;
    held_frame previous;
    held_frame waiting;
    double last_pts;
    bool fast;
    bool has_grid;
    double grid;
    double display_fps;
};

static const held_frame NO_FRAME = {NULL, 0.0, false};

static bool run(mb_schedule *s, mb_action *action)
{
    return s->sink.run(s->sink.context, action);
}

static void notify(mb_schedule *s, mb_action_kind kind)
{
    mb_action action = {kind, NULL, 0.0, 0.0, 0.0};
    run(s, &action);
}

static void announce(mb_schedule *s, mb_action action)
{
    run(s, &action);
}

static void release(mb_schedule *s, held_frame *held)
{
    if (held->present) {
        announce(s, (mb_action){MB_ACTION_RELEASE, held->frame, held->pts, 0.0, 0.0});
    }
    *held = NO_FRAME;
}

mb_schedule *mb_schedule_create(const mb_schedule_config *config, mb_action_sink sink)
{
    mb_schedule *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->config = *config;
    s->sink = sink;
    s->last_pts = NAN;
    return s;
}

void mb_schedule_destroy(mb_schedule *schedule)
{
    if (schedule) {
        mb_schedule_reset(schedule);
        free(schedule);
    }
}

void mb_schedule_reset(mb_schedule *s)
{
    mb_schedule_forget_frames(s);
    s->has_grid = false;
    s->last_pts = NAN;
    s->fast = false;
}

void mb_schedule_forget_frames(mb_schedule *s)
{
    release(s, &s->previous);
    release(s, &s->waiting);
}

int mb_schedule_factor(const mb_schedule *s)
{
    return s->config.factor;
}

static bool pair_ok(const mb_schedule *s, double a, double b)
{
    double gap = b - a;
    const mb_schedule_config *c = &s->config;
    return !isnan(a) && !isnan(b) && gap > 0 && (c->max_gap <= 0 || gap <= c->max_gap) &&
           (c->max_fps <= 0 || 1.0 / gap <= c->max_fps);
}

static bool short_gap(double gap, double rate)
{
    return gap < RATE_TOLERANCE / rate;
}

typedef struct output_slot {
    double pts;
    double duration;
} output_slot;

static const double KEEP_DURATION = -1.0;

static void emit_real(mb_schedule *s, const held_frame *f, output_slot slot)
{
    announce(s, (mb_action){MB_ACTION_EMIT_REAL, f->frame, slot.pts, slot.duration, 0.0});
}

static void emit_interpolated(mb_schedule *s, const held_frame *reference, double t, output_slot slot)
{
    announce(s, (mb_action){MB_ACTION_EMIT_INTERPOLATED, reference->frame, slot.pts, slot.duration, t});
}

static double grid_step(const mb_schedule *s, double gap, double rate)
{
    double step = 1.0 / rate;
    int per = (int)(gap / step + 0.5);
    int stride = (per + s->config.max_pending) / (s->config.max_pending + 1);
    return stride > 1 ? step * stride : step;
}

static void fill_display_grid(mb_schedule *s, const held_frame *current, double rate)
{
    const held_frame *previous = &s->previous;
    double gap = current->pts - previous->pts;
    double step = grid_step(s, gap, rate);
    if (!s->has_grid || s->grid <= previous->pts || s->grid > current->pts + step) {
        s->grid = previous->pts + step;
        s->has_grid = true;
    }
    for (; s->grid < current->pts + step * GRID_SLACK; s->grid += step) {
        double t = (s->grid - previous->pts) / gap;
        output_slot slot = {s->grid, step};
        if (t > REAL_FRAME_T) {
            emit_real(s, current, slot);
        } else {
            emit_interpolated(s, t < 0.5 ? previous : current, t, slot);
        }
    }
}

static void emit_display_rate(mb_schedule *s, held_frame current, bool ok, double rate)
{
    double step = 1.0 / rate;
    double gap = s->previous.present ? current.pts - s->previous.pts : 0;
    if (!ok || short_gap(gap, rate)) {
        emit_real(s, &current, (output_slot){current.pts, gap > 0 ? gap : step});
        s->grid = current.pts + step;
        s->has_grid = true;
    } else {
        fill_display_grid(s, &current, rate);
    }
    release(s, &s->previous);
    s->previous = current;
}

static int frames_per_pair(const mb_schedule *s, bool ok, double gap, double rate)
{
    int n = s->config.factor > 0 ? s->config.factor : 2;
    if (ok && rate > 0) {
        int fitting = (int)(rate * gap * RATE_TOLERANCE);
        n = fitting < n ? fitting : n;
    }
    return n;
}

static void emit_multiplied(mb_schedule *s, held_frame current, bool ok, double rate)
{
    double gap = s->previous.present ? current.pts - s->previous.pts : 0;
    int n = frames_per_pair(s, ok, gap, rate);
    double duration = KEEP_DURATION;
    if (ok && n > 1) {
        for (int k = 1; k < n; k++) {
            double t = (double)k / n;
            output_slot slot = {s->previous.pts + gap * t, gap / n};
            emit_interpolated(s, t < 0.5 ? &s->previous : &current, t, slot);
        }
        duration = gap / n;
    }
    release(s, &s->previous);
    s->previous = current;
    emit_real(s, &current, (output_slot){current.pts, duration});
}

static void emit_pair(mb_schedule *s, held_frame current)
{
    bool ok = s->previous.present && pair_ok(s, s->previous.pts, current.pts);
    double rate = s->display_fps;
    if (s->config.factor == MB_FACTOR_AUTO && rate > 0) {
        emit_display_rate(s, current, ok, rate);
    } else {
        emit_multiplied(s, current, ok, rate);
    }
}

static void flush_waiting(mb_schedule *s)
{
    if (!s->waiting.present) {
        return;
    }
    held_frame waiting = s->waiting;
    s->waiting = NO_FRAME;
    notify(s, MB_ACTION_END_STREAM);
    emit_pair(s, waiting);
}

static void drop_pair(mb_schedule *s)
{
    flush_waiting(s);
    release(s, &s->previous);
}

void mb_schedule_stop(mb_schedule *s)
{
    drop_pair(s);
    notify(s, MB_ACTION_DRAIN);
    s->has_grid = false;
}

void mb_schedule_set_factor(mb_schedule *s, int factor)
{
    if (factor == s->config.factor) {
        return;
    }
    s->config.factor = factor;
    s->has_grid = false;
    if (factor == MB_FACTOR_OFF) {
        mb_schedule_stop(s);
    }
}

static bool too_fast(const mb_schedule *s, double gap)
{
    double rate = s->display_fps;
    bool skipped_by_display = false;
    if (rate > 0) {
        skipped_by_display = s->config.factor == MB_FACTOR_AUTO ? short_gap(gap, rate)
                                                                : (int)(rate * gap * RATE_TOLERANCE) < 2;
    }
    return skipped_by_display || (s->config.max_fps > 0 && 1.0 / gap > s->config.max_fps);
}

static void pass_through(mb_schedule *s, held_frame frame, double gap)
{
    drop_pair(s);
    emit_real(s, &frame, (output_slot){frame.pts, gap});
    release(s, &frame);
}

static void start_pair_if_needed(mb_schedule *s)
{
    if (!s->previous.present) {
        notify(s, MB_ACTION_DRAIN);
        notify(s, MB_ACTION_RESET_ENGINE);
    }
}

static void schedule_pushed(mb_schedule *s, held_frame frame)
{
    if (!s->previous.present) {
        emit_pair(s, frame);
        return;
    }
    held_frame waiting = s->waiting;
    s->waiting = frame;
    if (waiting.present) {
        emit_pair(s, waiting);
    }
}

static void push_and_schedule(mb_schedule *s, held_frame frame)
{
    start_pair_if_needed(s);
    mb_action push = {MB_ACTION_PUSH, frame.frame, frame.pts, 0.0, 0.0};
    if (!run(s, &push)) {
        drop_pair(s);
        return;
    }
    frame.frame = push.frame;
    schedule_pushed(s, frame);
}

static void pass_unchanged(mb_schedule *s, held_frame frame)
{
    announce(s, (mb_action){MB_ACTION_PASS, frame.frame, frame.pts, KEEP_DURATION, 0.0});
    release(s, &frame);
}

void mb_schedule_frame(mb_schedule *s, mb_timed_frame input, double display_fps)
{
    held_frame frame = {input.frame, input.pts, true};
    s->display_fps = display_fps;
    if (s->config.factor == MB_FACTOR_OFF) {
        pass_unchanged(s, frame);
        return;
    }
    double last = s->last_pts;
    double gap = frame.pts - last;
    s->last_pts = frame.pts;
    bool fast = !isnan(last) && !isnan(frame.pts) && gap > 0 && too_fast(s, gap);
    bool bypass = fast && s->fast;
    s->fast = fast;
    if (bypass) {
        pass_through(s, frame, gap);
        return;
    }
    if (s->previous.present && !fast && !pair_ok(s, last, frame.pts)) {
        drop_pair(s);
    }
    push_and_schedule(s, frame);
}

void mb_schedule_end(mb_schedule *s, double display_fps)
{
    s->display_fps = display_fps;
    drop_pair(s);
}
