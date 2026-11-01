#include "check.h"
#include "framing/bar_tracker.h"
#include "framing/bars.h"
#include "frames.h"

enum { W = 320, H = 240, BAR = 40 };

static bool letterboxed(test_frame *f, int bar)
{
    if (!test_frame_alloc(f, W, H)) {
        return false;
    }
    paint_texture(f, (test_motion){0, 0, 7});
    paint_bars(f, bar);
    return true;
}

static bool finds_letterbox(void)
{
    test_frame f;
    CHECK(letterboxed(&f, BAR));
    bar_source source = {&f.format, &f.image, NULL};
    mb_rect active;
    CHECK(bars_detect(&source, &active));
    CHECK(mb_rect_equal(active, (mb_rect){0, BAR, W, H - 2 * BAR}));
    test_frame_free(&f);
    return true;
}

static bool keeps_full_frame_without_bars(void)
{
    test_frame f;
    CHECK(letterboxed(&f, 0));
    bar_source source = {&f.format, &f.image, NULL};
    mb_rect active;
    CHECK(bars_detect(&source, &active));
    CHECK(mb_rect_equal(active, (mb_rect){0, 0, W, H}));
    test_frame_free(&f);
    return true;
}

static bool gives_up_on_black_frames(void)
{
    test_frame f;
    CHECK(letterboxed(&f, H / 2));
    bar_source source = {&f.format, &f.image, NULL};
    mb_rect active;
    CHECK(!bars_detect(&source, &active));
    test_frame_free(&f);
    return true;
}

static bool reads_device_flags(void)
{
    uint32_t flags[W + H];
    for (int i = 0; i < W + H; i++) {
        flags[i] = 0;
    }
    for (int y = 0; y < 16; y++) {
        flags[y] = 1;
    }
    mb_video_format format = {W, H, 8, 1, 1, true, 0};
    bar_source source = {&format, NULL, flags};
    mb_rect active;
    CHECK(bars_detect(&source, &active));
    CHECK(mb_rect_equal(active, (mb_rect){0, 16, W, H - 16}));
    return true;
}

static bar_decision observe(bar_tracker *t, const test_frame *f, mb_rect active, int pushes)
{
    bar_source source = {&f->format, &f->image, NULL};
    bar_observation o = {&source, active, pushes, 3};
    return bar_tracker_update(t, &o);
}

static bool crops_at_first_sight(void)
{
    test_frame f;
    CHECK(letterboxed(&f, BAR));
    bar_tracker tracker = {0};
    bar_decision d = observe(&tracker, &f, (mb_rect){0, 0, W, H}, 1);
    CHECK(d.crop && mb_rect_equal(d.active, (mb_rect){0, BAR, W, H - 2 * BAR}));
    test_frame_free(&f);
    return true;
}

static bool grows_at_once_and_shrinks_after_settling(void)
{
    test_frame wide, narrow;
    CHECK(letterboxed(&wide, 8) && letterboxed(&narrow, BAR));
    bar_tracker tracker = {.seen = true};
    mb_rect cropped = {0, BAR, W, H - 2 * BAR};
    bar_decision d = {false, cropped};
    int push = 1;
    for (; push <= 4 && !d.crop; push++) {
        d = observe(&tracker, &wide, cropped, push);
    }
    CHECK(d.crop && mb_rect_equal(d.active, (mb_rect){0, 8, W, H - 16}));
    int settled_at = 0;
    for (int first = push; push < first + 60 && !settled_at; push++) {
        settled_at = observe(&tracker, &narrow, d.active, push).crop ? push - first : 0;
    }
    CHECK(settled_at >= 3);
    test_frame_free(&wide);
    test_frame_free(&narrow);
    return true;
}

int main(void)
{
    static const test_case tests[] = {
        {"finds letterbox", finds_letterbox},
        {"keeps full frame without bars", keeps_full_frame_without_bars},
        {"gives up on black frames", gives_up_on_black_frames},
        {"reads device flags", reads_device_flags},
        {"crops at first sight", crops_at_first_sight},
        {"grows at once and shrinks after settling", grows_at_once_and_shrinks_after_settling},
    };
    return RUN_ALL(tests);
}
