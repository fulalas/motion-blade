#include <math.h>
#include <string.h>

#include "check.h"
#include "motionblade/schedule.h"

enum { MAX_RECORDED = 256 };

typedef struct recorder {
    mb_action actions[MAX_RECORDED];
    int count;
    bool fail_push;
} recorder;

static bool record(void *context, mb_action *action)
{
    recorder *r = context;
    if (r->count < MAX_RECORDED) {
        r->actions[r->count++] = *action;
    }
    return !(action->kind == MB_ACTION_PUSH && r->fail_push);
}

static mb_schedule *schedule_with(recorder *r, int factor)
{
    memset(r, 0, sizeof(*r));
    mb_schedule_config config = {factor, 0, 0.25, 7};
    return mb_schedule_create(&config, (mb_action_sink){record, r});
}

static int count_kind(const recorder *r, mb_action_kind kind)
{
    int n = 0;
    for (int i = 0; i < r->count; i++) {
        n += r->actions[i].kind == kind;
    }
    return n;
}

static const mb_action *nth_kind(const recorder *r, mb_action_kind kind, int n)
{
    for (int i = 0; i < r->count; i++) {
        if (r->actions[i].kind == kind && n-- == 0) {
            return &r->actions[i];
        }
    }
    return NULL;
}

static void feed(mb_schedule *s, int count, double fps, double display)
{
    static int handles[64];
    for (int i = 0; i < count; i++) {
        mb_schedule_frame(s, (mb_timed_frame){&handles[i], i / fps}, display);
    }
}

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

static bool doubles_24p(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, 2);
    feed(s, 4, 24.0, 0.0);
    CHECK(count_kind(&r, MB_ACTION_PUSH) == 4);
    CHECK(count_kind(&r, MB_ACTION_EMIT_INTERPOLATED) == 2);
    const mb_action *first = nth_kind(&r, MB_ACTION_EMIT_INTERPOLATED, 0);
    CHECK(near(first->t, 0.5) && near(first->pts, 0.5 / 24.0) && near(first->duration, 0.5 / 24.0));
    mb_schedule_destroy(s);
    return true;
}

static bool fills_display_refresh(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, MB_FACTOR_AUTO);
    feed(s, 5, 24.0, 120.0);
    CHECK(count_kind(&r, MB_ACTION_EMIT_INTERPOLATED) == 12);
    const mb_action *first = nth_kind(&r, MB_ACTION_EMIT_INTERPOLATED, 0);
    CHECK(near(first->t, 0.2) && near(first->duration, 1.0 / 120.0));
    mb_schedule_destroy(s);
    return true;
}

static bool passes_through_display_rate(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, MB_FACTOR_AUTO);
    feed(s, 6, 60.0, 60.0);
    CHECK(count_kind(&r, MB_ACTION_EMIT_INTERPOLATED) == 0);
    CHECK(count_kind(&r, MB_ACTION_EMIT_REAL) == 6);
    mb_schedule_destroy(s);
    return true;
}

static bool restarts_after_gap(void)
{
    recorder r;
    static int handles[4];
    mb_schedule *s = schedule_with(&r, 2);
    mb_schedule_frame(s, (mb_timed_frame){&handles[0], 0.0}, 0.0);
    mb_schedule_frame(s, (mb_timed_frame){&handles[1], 0.04}, 0.0);
    mb_schedule_frame(s, (mb_timed_frame){&handles[2], 2.0}, 0.0);
    CHECK(count_kind(&r, MB_ACTION_RESET_ENGINE) == 2);
    CHECK(count_kind(&r, MB_ACTION_END_STREAM) == 1);
    mb_schedule_destroy(s);
    return true;
}

static bool flushes_last_pair_at_end(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, 2);
    feed(s, 3, 24.0, 0.0);
    int before = count_kind(&r, MB_ACTION_EMIT_INTERPOLATED);
    mb_schedule_end(s, 0.0);
    CHECK(count_kind(&r, MB_ACTION_END_STREAM) == 1);
    CHECK(count_kind(&r, MB_ACTION_EMIT_INTERPOLATED) == before + 1);
    mb_schedule_destroy(s);
    return true;
}

static bool releases_every_frame(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, 2);
    feed(s, 5, 24.0, 0.0);
    mb_schedule_end(s, 0.0);
    CHECK(count_kind(&r, MB_ACTION_RELEASE) == 5);
    mb_schedule_destroy(s);
    return true;
}

static bool stops_on_failed_push(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, 2);
    feed(s, 2, 24.0, 0.0);
    r.fail_push = true;
    static int lost;
    mb_schedule_frame(s, (mb_timed_frame){&lost, 3 / 24.0}, 0.0);
    CHECK(count_kind(&r, MB_ACTION_RELEASE) == 2);
    mb_schedule_destroy(s);
    return true;
}

static bool off_passes_frames_unchanged(void)
{
    recorder r;
    mb_schedule *s = schedule_with(&r, MB_FACTOR_OFF);
    feed(s, 3, 24.0, 60.0);
    CHECK(count_kind(&r, MB_ACTION_PASS) == 3);
    CHECK(count_kind(&r, MB_ACTION_PUSH) == 0);
    mb_schedule_destroy(s);
    return true;
}

int main(void)
{
    static const test_case tests[] = {
        {"doubles 24p", doubles_24p},
        {"fills display refresh", fills_display_refresh},
        {"passes through display rate", passes_through_display_rate},
        {"restarts after gap", restarts_after_gap},
        {"flushes last pair at end", flushes_last_pair_at_end},
        {"releases every frame", releases_every_frame},
        {"stops on failed push", stops_on_failed_push},
        {"off passes frames unchanged", off_passes_frames_unchanged},
    };
    return RUN_ALL(tests);
}
