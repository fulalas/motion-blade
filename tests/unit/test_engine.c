#include "check.h"
#include "frames.h"
#include "pixel_formats.h"
#include "motionblade/cpu_backend.h"
#include "motionblade/engine.h"

enum { WIDTH = 192, HEIGHT = 128, MARGIN = 24 };

static mb_engine *cpu_engine(int width, int height)
{
    mb_engine_config config = {
        .format = test_format_video(test_format_default(), width, height),
        .logger = mb_logger_stderr(MB_LOG_WARNING),
        .params = mb_params_default(),
    };
    mb_engine *engine = NULL;
    if (mb_cpu_backend_create(&config.backend) != MB_SUCCESS || mb_engine_create(&config, &engine) != MB_SUCCESS) {
        return NULL;
    }
    return engine;
}

static bool push_motion(mb_engine *engine, test_frame *scratch, test_motion motion)
{
    paint_texture(scratch, motion);
    return mb_engine_push(engine, &scratch->image) == MB_SUCCESS;
}

static bool interpolate(mb_engine *engine, double t, test_frame *out, mb_frame_report *report)
{
    return mb_engine_submit(engine, t) == MB_SUCCESS && mb_engine_retrieve(engine, &out->image, report) == MB_SUCCESS;
}

static mb_rect whole_frame(void)
{
    return (mb_rect){0, 0, WIDTH, HEIGHT};
}

static mb_rect interior(void)
{
    return (mb_rect){MARGIN, MARGIN, WIDTH - 2 * MARGIN, HEIGHT - 2 * MARGIN};
}

static bool moving_texture_lands_halfway(void)
{
    mb_engine *engine = cpu_engine(WIDTH, HEIGHT);
    test_frame frame, out, truth;
    CHECK(engine && test_frame_alloc(&frame, WIDTH, HEIGHT) && test_frame_alloc(&out, WIDTH, HEIGHT));
    CHECK(test_frame_alloc(&truth, WIDTH, HEIGHT));
    for (int k = 0; k < 3; k++) {
        CHECK(push_motion(engine, &frame, (test_motion){4 * k, 2 * k, 1}));
    }
    mb_frame_report report;
    CHECK(interpolate(engine, 0.5, &out, &report));
    paint_texture(&truth, (test_motion){2, 1, 1});
    CHECK(!report.scene_cut);
    CHECK(luma_psnr(&out, &truth, interior()) > 40.0);
    test_frame_free(&frame);
    test_frame_free(&out);
    test_frame_free(&truth);
    mb_engine_destroy(engine);
    return true;
}

static bool still_scene_stays_still(void)
{
    mb_engine *engine = cpu_engine(WIDTH, HEIGHT);
    test_frame frame, out;
    CHECK(engine && test_frame_alloc(&frame, WIDTH, HEIGHT) && test_frame_alloc(&out, WIDTH, HEIGHT));
    for (int k = 0; k < 3; k++) {
        CHECK(push_motion(engine, &frame, (test_motion){0, 0, 2}));
    }
    mb_frame_report report;
    CHECK(interpolate(engine, 0.5, &out, &report));
    CHECK(luma_psnr(&out, &frame, whole_frame()) > 50.0);
    test_frame_free(&frame);
    test_frame_free(&out);
    mb_engine_destroy(engine);
    return true;
}

static bool cut_output_matches(const test_motion *shots, int count, double t, test_motion expected, mb_rect area,
                               double min_psnr)
{
    mb_engine *engine = cpu_engine(WIDTH, HEIGHT);
    test_frame frame, out, truth;
    CHECK(engine && test_frame_alloc(&frame, WIDTH, HEIGHT) && test_frame_alloc(&out, WIDTH, HEIGHT));
    CHECK(test_frame_alloc(&truth, WIDTH, HEIGHT));
    mb_frame_report report;
    for (int k = 0; k < count; k++) {
        CHECK(push_motion(engine, &frame, shots[k]));
        if (k >= 2) {
            CHECK(interpolate(engine, k == count - 1 ? t : 0.5, &out, &report));
        }
    }
    CHECK(report.scene_cut);
    paint_texture(&truth, expected);
    CHECK(luma_psnr(&out, &truth, area) >= min_psnr);
    test_frame_free(&frame);
    test_frame_free(&out);
    test_frame_free(&truth);
    mb_engine_destroy(engine);
    return true;
}

static const test_motion PANNING_CUT[4] = {{0, 0, 3}, {4, 0, 3}, {0, 0, 9}, {4, 0, 9}};

static bool scene_cut_moves_old_frame_forward(void)
{
    return cut_output_matches(PANNING_CUT, 4, 0.25, (test_motion){5, 0, 3}, interior(), 40.0);
}

static bool scene_cut_at_middle_moves_old_frame_forward(void)
{
    return cut_output_matches(PANNING_CUT, 4, 0.5, (test_motion){6, 0, 3}, interior(), 40.0);
}

static bool scene_cut_moves_new_frame_back(void)
{
    return cut_output_matches(PANNING_CUT, 4, 0.75, (test_motion){-1, 0, 9}, interior(), 40.0);
}

static bool scene_cut_without_new_motion_moves_old_frame(void)
{
    test_motion shots[4] = {{0, 0, 3}, {4, 0, 3}, {0, 0, 9}, {4, 0, 17}};
    return cut_output_matches(shots, 4, 0.75, (test_motion){7, 0, 3}, interior(), 40.0);
}

static bool scene_cut_without_old_motion_moves_new_frame(void)
{
    test_motion shots[6] = {{0, 0, 3}, {4, 0, 3}, {8, 0, 3}, {0, 0, 17}, {0, 0, 9}, {4, 0, 9}};
    return cut_output_matches(shots, 6, 0.25, (test_motion){-3, 0, 9}, interior(), 40.0);
}

static const test_motion CUTS_IN_A_ROW[5] = {{0, 0, 3}, {4, 0, 3}, {0, 0, 17}, {0, 0, 9}, {0, 0, 25}};

static bool scene_cut_without_any_motion_copies_old_frame(void)
{
    return cut_output_matches(CUTS_IN_A_ROW, 5, 0.25, (test_motion){0, 0, 17}, whole_frame(), 99.0);
}

static bool scene_cut_without_any_motion_copies_new_frame(void)
{
    return cut_output_matches(CUTS_IN_A_ROW, 5, 0.75, (test_motion){0, 0, 9}, whole_frame(), 99.0);
}

static bool letterbox_bars_survive(void)
{
    enum { BAR = 24 };
    mb_engine *engine = cpu_engine(WIDTH, HEIGHT);
    test_frame frame, out;
    CHECK(engine && test_frame_alloc(&frame, WIDTH, HEIGHT) && test_frame_alloc(&out, WIDTH, HEIGHT));
    for (int k = 0; k < 3; k++) {
        paint_texture(&frame, (test_motion){4 * k, 0, 4});
        paint_bars(&frame, BAR);
        CHECK(mb_engine_push(engine, &frame.image) == MB_SUCCESS);
    }
    mb_frame_report report;
    CHECK(interpolate(engine, 0.5, &out, &report));
    CHECK(luma_rows_equal(&out, &frame, 0, BAR));
    CHECK(luma_rows_equal(&out, &frame, HEIGHT - BAR, BAR));
    test_frame_free(&frame);
    test_frame_free(&out);
    mb_engine_destroy(engine);
    return true;
}

static bool waits_for_lookahead(void)
{
    mb_engine *engine = cpu_engine(WIDTH, HEIGHT);
    test_frame frame;
    CHECK(engine && test_frame_alloc(&frame, WIDTH, HEIGHT));
    CHECK(push_motion(engine, &frame, (test_motion){0, 0, 5}));
    CHECK(push_motion(engine, &frame, (test_motion){1, 0, 5}));
    CHECK(mb_engine_submit(engine, 0.5) == MB_ERROR_BUSY);
    mb_engine_end_stream(engine);
    CHECK(mb_engine_submit(engine, 0.5) == MB_SUCCESS);
    CHECK(mb_engine_pending(engine) == 1);
    test_frame_free(&frame);
    mb_engine_destroy(engine);
    return true;
}

static bool rejects_tiny_frames(void)
{
    CHECK(cpu_engine(32, 32) == NULL);
    return true;
}

int main(void)
{
    static const test_case tests[] = {
        {"moving texture lands halfway", moving_texture_lands_halfway},
        {"still scene stays still", still_scene_stays_still},
        {"scene cut moves old frame forward", scene_cut_moves_old_frame_forward},
        {"scene cut at middle moves old frame forward", scene_cut_at_middle_moves_old_frame_forward},
        {"scene cut moves new frame back", scene_cut_moves_new_frame_back},
        {"scene cut without new motion moves old frame", scene_cut_without_new_motion_moves_old_frame},
        {"scene cut without old motion moves new frame", scene_cut_without_old_motion_moves_new_frame},
        {"scene cut without any motion copies old frame", scene_cut_without_any_motion_copies_old_frame},
        {"scene cut without any motion copies new frame", scene_cut_without_any_motion_copies_new_frame},
        {"letterbox bars survive", letterbox_bars_survive},
        {"waits for lookahead", waits_for_lookahead},
        {"rejects tiny frames", rejects_tiny_frames},
    };
    return RUN_ALL(tests);
}
