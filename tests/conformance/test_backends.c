#include <stdio.h>

#include "check.h"
#include "frames.h"
#include "pixel_formats.h"
#include "motionblade/cpu_backend.h"
#include "motionblade/engine.h"
#include "motionblade/vulkan.h"
#include "tee_backend.h"

enum { FRAMES = 11, SKIP = 77 };

typedef struct expectation {
    const char *pass;
    double min_agreement;
    double max_difference;
} expectation;

static const expectation EXPECTED[] = {
    {"normalize", 0.999, 0.25},
    {"downsample", 0.999, 0.25},
    {"full_search", 0.995, 0.1},
    {"iterate", 0.995, 0.5},
    {"measure", 0.995, 0.1},
    {"measure_stats", 0.9, 0.0},
    {"project", 0.995, 0.0},
    {"cross_check", 0.995, 0.1},
    {"local_search", 0.995, 0.1},
    {"decide_cut", 1.0, 0.0},
    {"cut_history", 0.99, 0.01},
    {"select", 0.995, 0.01},
    {"median", 0.995, 0.0},
    {"fallback", 0.995, 0.001},
    {"chroma_offsets", 0.995, 0.001},
    {"offset_median", 0.995, 0.001},
    {"global_motion", 1.0, 0.0},
    {"panels", 0.995, 0.001},
    {"panel_spread", 0.995, 0.001},
    {"split", 0.995, 0.0},
    {"interpolate_luma", 0.99, 2.0},
    {"interpolate_chroma", 0.99, 2.0},
    {"output_cut", 1.0, 0.0},
};

typedef struct run_config {
    const char *name;
    int width;
    int height;
    bool interleaved;
    int bit_depth;
    int half_resolution;
    void (*paint)(test_frame *frame, int index);
} run_config;

static bool device_available = true;

static const test_format *layout_of(const run_config *run)
{
    if (run->bit_depth > 8) {
        return test_format_find("yuv420p10");
    }
    return test_format_find(run->interleaved ? "nv12" : "yuv420p");
}

static mb_status tee_engine(const run_config *run, tee_report *report, mb_engine **engine)
{
    mb_backend reference, device;
    mbvk_config vk = {.logger = mb_logger_stderr(MB_LOG_ERROR)};
    if (mb_cpu_backend_create(&reference) != MB_SUCCESS) {
        return MB_ERROR_NO_MEMORY;
    }
    if (mbvk_backend_create(&vk, &device, NULL) != MB_SUCCESS) {
        reference.ops->destroy(reference.self);
        device_available = false;
        return MB_ERROR_UNSUPPORTED;
    }
    mb_engine_config config = {
        .format = test_format_video(layout_of(run), run->width, run->height),
        .logger = mb_logger_stderr(MB_LOG_WARNING),
        .params = mb_params_default(),
    };
    config.params.search.half_resolution = run->half_resolution;
    mb_status status = tee_backend_create(reference, device, report, &config.backend);
    return status == MB_SUCCESS ? mb_engine_create(&config, engine) : status;
}

static bool play(mb_engine *engine, const run_config *run)
{
    test_frame planar;
    converted_frame input, out;
    CHECK(test_frame_alloc(&planar, run->width, run->height));
    CHECK(converted_alloc(&input, layout_of(run), run->width, run->height));
    CHECK(converted_alloc(&out, layout_of(run), run->width, run->height));
    for (int k = 0; k < FRAMES; k++) {
        (run->paint ? run->paint : paint_scene)(&planar, k);
        convert_from_yuv420(&input, planar.storage);
        CHECK(mb_engine_push(engine, &input.image) == MB_SUCCESS);
        mb_frame_report report;
        if (k >= 2) {
            CHECK(mb_engine_submit(engine, k % 2 ? 0.5 : 0.3) == MB_SUCCESS);
            CHECK(mb_engine_retrieve(engine, &out.image, &report) == MB_SUCCESS);
        }
    }
    test_frame_free(&planar);
    converted_free(&input);
    converted_free(&out);
    return true;
}

static void print_report(const char *name, const tee_report *report)
{
    printf("%s\n", name);
    for (int i = 0; i < report->count; i++) {
        const tee_pass_report *p = &report->passes[i];
        printf("  %-20s runs %4d  agreement %.5f  max difference %.6g\n", p->name, p->runs, p->worst_agreement,
               p->worst_difference);
    }
}

static bool meets_expectations(const tee_report *report)
{
    bool ok = true;
    for (size_t i = 0; i < sizeof(EXPECTED) / sizeof(EXPECTED[0]); i++) {
        const expectation *e = &EXPECTED[i];
        const tee_pass_report *p = tee_find(report, e->pass);
        if (!p) {
            continue;
        }
        if (p->worst_agreement < e->min_agreement || p->worst_difference > e->max_difference) {
            fprintf(stderr, "%s: agreement %.5f (min %.5f), difference %g (max %g)\n", e->pass, p->worst_agreement,
                    e->min_agreement, p->worst_difference, e->max_difference);
            ok = false;
        }
    }
    return ok;
}

static bool backends_agree(const run_config *run)
{
    static tee_report report;
    mb_engine *engine = NULL;
    mb_status status = tee_engine(run, &report, &engine);
    if (status == MB_ERROR_UNSUPPORTED && !device_available) {
        return true;
    }
    CHECK(status == MB_SUCCESS);
    bool played = play(engine, run);
    mb_engine_destroy(engine);
    print_report(run->name, &report);
    CHECK(played);
    CHECK(report.count > 0);
    CHECK(meets_expectations(&report));
    return true;
}

static bool planar_frames_agree(void)
{
    return backends_agree(&(run_config){"planar", 256, 160, false, 8, 0, NULL});
}

static bool interleaved_frames_agree(void)
{
    return backends_agree(&(run_config){"interleaved", 256, 160, true, 8, 0, NULL});
}

static bool ten_bit_frames_agree(void)
{
    return backends_agree(&(run_config){"ten bit", 256, 160, false, 10, 0, NULL});
}

static bool half_resolution_agrees(void)
{
    return backends_agree(&(run_config){"half resolution", 384, 224, false, 8, 1, NULL});
}

static bool cuts_in_a_row_agree(void)
{
    return backends_agree(&(run_config){"cuts in a row", 256, 160, false, 8, 0, paint_cuts_in_a_row});
}

int main(void)
{
    static const test_case tests[] = {
        {"planar frames agree", planar_frames_agree},
        {"interleaved frames agree", interleaved_frames_agree},
        {"ten bit frames agree", ten_bit_frames_agree},
        {"half resolution agrees", half_resolution_agrees},
        {"cuts in a row agree", cuts_in_a_row_agree},
    };
    int result = RUN_ALL(tests);
    return device_available ? result : SKIP;
}
