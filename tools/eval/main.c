#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "field_dump.h"
#include "metrics.h"
#include "motionblade/cpu_backend.h"
#include "motionblade/engine.h"
#include "motionblade/vulkan.h"
#include "options.h"
#include "pixel_formats.h"
#include "sequence.h"

typedef struct totals {
    double local;
    double chroma;
    double psnr;
    double ssim;
    double gpu_ms;
    double worst;
    int worst_frame;
    int count;
} totals;

typedef struct evaluation {
    const eval_options *options;
    const sequence *frames;
    const sequence *truth;
    mb_engine *engine;
    converted_frame input;
    converted_frame output;
    uint8_t *result;
    FILE *out;
    totals sum;
} evaluation;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static bool converts(const evaluation *ev)
{
    return ev->options->format != test_format_default();
}

static mb_status create_backend(const char *name, mb_backend *backend)
{
    if (!strcmp(name, "cpu")) {
        return mb_cpu_backend_create(backend);
    }
    if (!strcmp(name, "vulkan")) {
        mbvk_config config = {.logger = mb_logger_stderr(MB_LOG_WARNING), .profile = getenv("MOTIONBLADE_PROFILE")};
        return mbvk_backend_create(&config, backend, NULL);
    }
    return MB_ERROR_INVALID;
}

static mb_engine *create_engine(const eval_options *o, int width, int height)
{
    mb_engine_config config = {
        .format = test_format_video(o->format, width, height),
        .logger = mb_logger_stderr(getenv("MOTIONBLADE_VERBOSE") ? MB_LOG_VERBOSE : MB_LOG_WARNING),
        .params = mb_params_default(),
    };
    mb_status status = create_backend(o->backend, &config.backend);
    mb_engine *engine = NULL;
    if (status == MB_SUCCESS) {
        status = mb_engine_create(&config, &engine);
    }
    if (status != MB_SUCCESS) {
        fprintf(stderr, "cannot create the %s interpolator: %s\n", o->backend, mb_status_name(status));
        return NULL;
    }
    return engine;
}

static bool apply_params(const eval_options *o, mb_engine *engine)
{
    for (int i = 0; i < o->param_count; i++) {
        char *eq = strchr(o->params[i], '=');
        if (!eq) {
            fprintf(stderr, "parameter %s needs name=value\n", o->params[i]);
            return false;
        }
        *eq = 0;
        if (mb_engine_set_param(engine, o->params[i], atof(eq + 1)) != MB_SUCCESS) {
            fprintf(stderr, "unknown param %s\n", o->params[i]);
            return false;
        }
    }
    return true;
}

static void push_frame(evaluation *ev, uint8_t *frame)
{
    if (converts(ev)) {
        convert_from_yuv420(&ev->input, frame);
        mb_engine_push(ev->engine, &ev->input.image);
        return;
    }
    mb_host_image image = yuv420_image(frame, ev->frames->width, ev->frames->height);
    mb_engine_push(ev->engine, &image);
}

static void feed_pair(evaluation *ev, int k)
{
    int step = ev->options->step;
    for (int j = k == 0 ? 0 : k + 2; j <= k + 2; j++) {
        if (j * step >= ev->frames->count) {
            mb_engine_end_stream(ev->engine);
            continue;
        }
        push_frame(ev, ev->frames->frames[j * step]);
    }
}

static bool interpolate(evaluation *ev, double t, mb_frame_report *report)
{
    mb_host_image target = yuv420_image(ev->result, ev->frames->width, ev->frames->height);
    const mb_host_image *destination = converts(ev) ? &ev->output.image : &target;
    if (mb_engine_submit(ev->engine, t) != MB_SUCCESS ||
        mb_engine_retrieve(ev->engine, destination, report) != MB_SUCCESS) {
        return false;
    }
    if (converts(ev)) {
        convert_to_yuv420(&ev->output, ev->result);
    }
    return true;
}

static void make_baseline(evaluation *ev, const uint8_t *a, const uint8_t *b, double t)
{
    size_t bytes = frame_bytes(ev->frames->width, ev->frames->height);
    if (ev->options->mode == MODE_BLEND) {
        for (size_t i = 0; i < bytes; i++) {
            ev->result[i] = (uint8_t)lrint(a[i] * (1 - t) + b[i] * t);
        }
    } else {
        memcpy(ev->result, t < 0.5 ? a : b, bytes);
    }
}

static void score(evaluation *ev, const uint8_t *frame, int index, const mb_frame_report *report)
{
    int w = ev->frames->width;
    int h = ev->frames->height;
    size_t luma = (size_t)w * (size_t)h;
    const uint8_t *truth = ev->frames->frames[index];
    bool measure = !ev->options->no_metrics;
    double psnr = measure ? metric_psnr(frame, truth, luma) : 0;
    double ssim = measure ? metric_ssim(frame, truth, w, h) : 0;
    double local = measure ? metric_worst_blocks(frame, truth, w, h) : 0;
    if (!ev->options->quiet) {
        printf("frame %3d  local %6.2f  psnr %6.2f  ssim %.4f  gpu %6.2fms  err %.1f%s\n", index, local, psnr, ssim,
               report->gpu_ms, report->mean_error, report->scene_cut ? " CUT" : "");
    }
    totals *s = &ev->sum;
    s->psnr += psnr;
    s->chroma += measure ? metric_psnr(frame + luma, truth + luma, luma / 2) : 0;
    s->local += local;
    s->ssim += ssim;
    s->gpu_ms += report->gpu_ms;
    if (psnr < s->worst) {
        s->worst = psnr;
        s->worst_frame = index;
    }
    s->count++;
}

static void write_frame(evaluation *ev, const uint8_t *frame)
{
    if (ev->out) {
        fprintf(ev->out, "FRAME\n");
        fwrite(frame, 1, frame_bytes(ev->frames->width, ev->frames->height), ev->out);
    }
}

static bool make_frame(evaluation *ev, int k, int m, mb_frame_report *report)
{
    const eval_options *o = ev->options;
    int index = k * o->step + m;
    double t = (double)m / o->step;
    if (o->compare) {
        if (index >= ev->truth->count) {
            fprintf(stderr, "%s has only %d frames\n", o->compare, ev->truth->count);
            return false;
        }
        memcpy(ev->result, ev->truth->frames[index], frame_bytes(ev->frames->width, ev->frames->height));
        return true;
    }
    if (o->mode != MODE_INTERPOLATE) {
        make_baseline(ev, ev->frames->frames[k * o->step], ev->frames->frames[(k + 1) * o->step], t);
        return true;
    }
    if (!interpolate(ev, t, report)) {
        fprintf(stderr, "interpolation failed at frame %d\n", index);
        return false;
    }
    if (index == o->dump_frame) {
        dump_fields(ev->engine, o->dump_path ? o->dump_path : "/tmp/motionblade-fields.ppm", ev->frames->width,
                    ev->frames->height);
    }
    return true;
}

static bool run_pair(evaluation *ev, int k)
{
    const eval_options *o = ev->options;
    if (o->mode == MODE_INTERPOLATE) {
        feed_pair(ev, k);
    }
    write_frame(ev, ev->frames->frames[k * o->step]);
    for (int m = 1; m < o->step; m++) {
        mb_frame_report report = {0};
        if (!make_frame(ev, k, m, &report)) {
            return false;
        }
        score(ev, ev->result, k * o->step + m, &report);
        write_frame(ev, ev->result);
    }
    return true;
}

static bool run(evaluation *ev)
{
    const eval_options *o = ev->options;
    for (int round = 0; round < o->repeat; round++) {
        for (int k = 0; (k + 1) * o->step < ev->frames->count; k++) {
            if (k == 0 && round > 0) {
                mb_engine_reset(ev->engine);
            }
            if (!run_pair(ev, k)) {
                return false;
            }
        }
    }
    return true;
}

static void print_summary(const evaluation *ev, double wall_ms)
{
    const totals *s = &ev->sum;
    if (ev->options->no_metrics) {
        printf("wall %.2f ms per interpolated frame\n", wall_ms / s->count);
    }
    printf("MEAN local %.3f | chroma psnr %.3f | ", s->local / s->count, s->chroma / s->count);
    printf("MEAN psnr %.3f ssim %.4f  worst %.2f@%d  gpu %.2fms  (%d frames)\n", s->psnr / s->count,
           s->ssim / s->count, s->worst, s->worst_frame, s->gpu_ms / s->count, s->count);
}

static bool allocate_conversions(evaluation *ev, int width, int height)
{
    const test_format *format = ev->options->format;
    return converted_alloc(&ev->input, format, width, height) && converted_alloc(&ev->output, format, width, height);
}

static bool prepare(evaluation *ev)
{
    const eval_options *o = ev->options;
    int w = ev->frames->width;
    int h = ev->frames->height;
    ev->result = calloc(1, frame_bytes(w, h));
    if (!ev->result) {
        return false;
    }
    if (converts(ev) && !allocate_conversions(ev, w, h)) {
        return false;
    }
    if (o->mode == MODE_INTERPOLATE) {
        ev->engine = create_engine(o, w, h);
        if (!ev->engine || !apply_params(o, ev->engine)) {
            return false;
        }
    }
    if (o->output) {
        ev->out = fopen(o->output, "wb");
        if (!ev->out) {
            fprintf(stderr, "cannot write %s\n", o->output);
            return false;
        }
        fputs(ev->frames->header, ev->out);
    }
    return true;
}

static void finish(evaluation *ev)
{
    if (ev->out) {
        fclose(ev->out);
    }
    mb_engine_destroy(ev->engine);
    converted_free(&ev->input);
    converted_free(&ev->output);
    free(ev->result);
}

static int evaluate(const eval_options *o, const sequence *frames, const sequence *truth)
{
    evaluation ev = {.options = o, .frames = frames, .truth = truth, .sum = {.worst = 99, .worst_frame = -1}};
    int code = 1;
    if (prepare(&ev)) {
        double start = now_ms();
        bool ok = run(&ev);
        double wall = now_ms() - start;
        if (ok && ev.sum.count == 0) {
            fprintf(stderr, "no frame interpolated: %d frames read, need more than %d with -step %d\n", frames->count,
                    o->step, o->step);
        } else if (ok) {
            print_summary(&ev, wall);
            code = 0;
        }
    }
    finish(&ev);
    return code;
}

static bool load(const char *path, const eval_options *o, sequence *s)
{
    sequence_request request = {path, o->max_frames, o->skip};
    if (sequence_read(&request, s) < 0) {
        fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    eval_options options;
    if (!options_parse(argc, argv, &options)) {
        options_usage();
        return 1;
    }
    sequence frames = {0};
    sequence truth = {0};
    if (!load(options.input, &options, &frames) || (options.compare && !load(options.compare, &options, &truth))) {
        return 1;
    }
    if (options.compare && (truth.width != frames.width || truth.height != frames.height)) {
        fprintf(stderr, "%s and %s differ in size\n", options.input, options.compare);
        return 1;
    }
    int code = evaluate(&options, &frames, &truth);
    sequence_free(&frames);
    sequence_free(&truth);
    return code;
}
