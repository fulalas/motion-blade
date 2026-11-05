#include <math.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "motionblade/pulldown.h"

enum { WIDTH = 64, HEIGHT = 8 };

static const mb_field_motion MOVING = {5.0, 5.0};
static const mb_field_motion REPEAT_TOP = {1.0, 5.0};
static const mb_field_motion REPEAT_BOTTOM = {5.0, 1.0};
static const mb_field_motion STILL = {0.3, 0.3};

static mb_pulldown feed_cycles(mb_pulldown_detector *d, const mb_field_motion *cycle, int length, int frames)
{
    mb_pulldown state = mb_pulldown_state(d);
    for (int i = 0; i < frames; i++) {
        state = mb_pulldown_push(d, cycle[i % length]);
    }
    return state;
}

static const mb_field_motion TOP_FIRST_CYCLE[] = {REPEAT_TOP, MOVING, REPEAT_BOTTOM, MOVING, MOVING};
static const mb_field_motion BOTTOM_FIRST_CYCLE[] = {REPEAT_BOTTOM, MOVING, REPEAT_TOP, MOVING, MOVING};

static bool measures_each_field(void)
{
    uint8_t a[WIDTH * HEIGHT], b[WIDTH * HEIGHT];
    memset(a, 100, sizeof(a));
    memcpy(b, a, sizeof(b));
    for (int y = 1; y < HEIGHT; y += 2) {
        memset(b + y * WIDTH, 110, WIDTH);
    }
    mb_luma_plane pa = {a, WIDTH, WIDTH, HEIGHT, 1, 255};
    mb_luma_plane pb = {b, WIDTH, WIDTH, HEIGHT, 1, 255};
    mb_field_motion m = mb_field_motion_measure(&pa, &pb);
    CHECK(m.top == 0.0);
    CHECK(fabs(m.bottom - 10.0) < 1e-9);
    return true;
}

static bool scales_deep_samples(void)
{
    uint16_t a[WIDTH * HEIGHT], b[WIDTH * HEIGHT];
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        a[i] = 400;
        b[i] = 440;
    }
    mb_luma_plane pa = {a, WIDTH * 2, WIDTH, HEIGHT, 2, 1023};
    mb_luma_plane pb = {b, WIDTH * 2, WIDTH, HEIGHT, 2, 1023};
    mb_field_motion m = mb_field_motion_measure(&pa, &pb);
    CHECK(fabs(m.top - 40.0 * 255.0 / 1023.0) < 1e-9);
    CHECK(fabs(m.bottom - m.top) < 1e-9);
    return true;
}

static bool finds_top_first(void)
{
    mb_pulldown_detector *d = mb_pulldown_create();
    CHECK(feed_cycles(d, TOP_FIRST_CYCLE, 5, 9) == MB_PULLDOWN_NONE);
    CHECK(feed_cycles(d, TOP_FIRST_CYCLE + 4, 1, 1) == MB_PULLDOWN_TOP_FIRST);
    mb_pulldown_destroy(d);
    return true;
}

static bool finds_bottom_first(void)
{
    mb_pulldown_detector *d = mb_pulldown_create();
    CHECK(feed_cycles(d, BOTTOM_FIRST_CYCLE, 5, 20) == MB_PULLDOWN_BOTTOM_FIRST);
    mb_pulldown_destroy(d);
    return true;
}

static bool ignores_video(void)
{
    static const mb_field_motion interlaced[] = {MOVING, REPEAT_TOP, REPEAT_BOTTOM};
    mb_pulldown_detector *d = mb_pulldown_create();
    CHECK(feed_cycles(d, &MOVING, 1, 100) == MB_PULLDOWN_NONE);
    CHECK(feed_cycles(d, interlaced, 3, 100) == MB_PULLDOWN_NONE);
    CHECK(feed_cycles(d, &STILL, 1, 100) == MB_PULLDOWN_NONE);
    mb_pulldown_destroy(d);
    return true;
}

static bool keeps_film_through_still_scenes_and_cuts(void)
{
    mb_pulldown_detector *d = mb_pulldown_create();
    feed_cycles(d, TOP_FIRST_CYCLE, 5, 40);
    CHECK(feed_cycles(d, &STILL, 1, 200) == MB_PULLDOWN_TOP_FIRST);
    CHECK(feed_cycles(d, TOP_FIRST_CYCLE + 2, 5, 3) == MB_PULLDOWN_TOP_FIRST);
    CHECK(feed_cycles(d, TOP_FIRST_CYCLE, 5, 40) == MB_PULLDOWN_TOP_FIRST);
    mb_pulldown_forget(d);
    CHECK(mb_pulldown_state(d) == MB_PULLDOWN_TOP_FIRST);
    mb_pulldown_destroy(d);
    return true;
}

static bool leaves_film_when_video_lasts(void)
{
    mb_pulldown_detector *d = mb_pulldown_create();
    feed_cycles(d, TOP_FIRST_CYCLE, 5, 40);
    CHECK(feed_cycles(d, &MOVING, 1, 5) == MB_PULLDOWN_TOP_FIRST);
    CHECK(feed_cycles(d, &MOVING, 1, 10) == MB_PULLDOWN_NONE);
    mb_pulldown_destroy(d);
    return true;
}

static bool keeps_film_through_other_cadences(void)
{
    static const mb_field_motion film25[] = {REPEAT_TOP, MOVING, MOVING, MOVING, REPEAT_BOTTOM, MOVING, MOVING,
                                             REPEAT_TOP, MOVING, MOVING};
    mb_pulldown_detector *d = mb_pulldown_create();
    feed_cycles(d, TOP_FIRST_CYCLE, 5, 40);
    CHECK(feed_cycles(d, film25, 10, 300) == MB_PULLDOWN_TOP_FIRST);
    mb_pulldown_destroy(d);
    return true;
}

static bool keeps_film_when_repeats_are_faint(void)
{
    static const mb_field_motion faint[] = {{1.6, 2.4}, MOVING, {2.4, 1.6}, MOVING, MOVING};
    mb_pulldown_detector *d = mb_pulldown_create();
    feed_cycles(d, TOP_FIRST_CYCLE, 5, 20);
    CHECK(feed_cycles(d, faint, 5, 100) == MB_PULLDOWN_TOP_FIRST);
    mb_pulldown_destroy(d);
    return true;
}

static bool finds_repeated_frames(void)
{
    CHECK(mb_frame_repeats(9.0, 1.0, 45.0));
    CHECK(mb_frame_repeats(NAN, 1.0, 12.0));
    CHECK(!mb_frame_repeats(9.0, 5.0, 10.0));
    CHECK(!mb_frame_repeats(1.5, 0.3, 1.5));
    CHECK(!mb_frame_repeats(NAN, 1.0, NAN));
    return true;
}

static bool switches_field_order(void)
{
    mb_pulldown_detector *d = mb_pulldown_create();
    feed_cycles(d, TOP_FIRST_CYCLE, 5, 40);
    CHECK(feed_cycles(d, BOTTOM_FIRST_CYCLE, 5, 40) == MB_PULLDOWN_BOTTOM_FIRST);
    mb_pulldown_destroy(d);
    return true;
}

int main(void)
{
    static const test_case tests[] = {
        {"measures each field", measures_each_field},
        {"scales deep samples", scales_deep_samples},
        {"finds top first", finds_top_first},
        {"finds bottom first", finds_bottom_first},
        {"ignores video", ignores_video},
        {"keeps film through still scenes and cuts", keeps_film_through_still_scenes_and_cuts},
        {"leaves film when video lasts", leaves_film_when_video_lasts},
        {"keeps film through other cadences", keeps_film_through_other_cadences},
        {"keeps film when repeats are faint", keeps_film_when_repeats_are_faint},
        {"finds repeated frames", finds_repeated_frames},
        {"switches field order", switches_field_order},
    };
    return RUN_ALL(tests);
}
