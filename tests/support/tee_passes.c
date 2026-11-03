#include "tee_internal.h"

static mb_pair_planes pair_side(mb_pair_planes pair, int side)
{
    return (mb_pair_planes){planes_side(pair.a, side), planes_side(pair.b, side)};
}

static void tee_normalize(void *self, const mb_normalize_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_normalize_pass p = *pass;
        p.source = IMAGE(pass->source);
        p.target = IMAGE(pass->target);
        OPS(t, side)->normalize(SELF(t, side), &p);
    }
    check_image(t, "normalize", pass->target);
}

static void tee_downsample(void *self, const mb_downsample_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_downsample_pass p = *pass;
        p.source = IMAGE(pass->source);
        p.target = IMAGE(pass->target);
        OPS(t, side)->downsample(SELF(t, side), &p);
    }
    check_image(t, "downsample", pass->target);
}

static void tee_full_search(void *self, const mb_full_search_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_full_search_pass p = *pass;
        p.origin = IMAGE(pass->origin);
        p.target = IMAGE(pass->target);
        p.field = BUFFER(pass->field);
        OPS(t, side)->full_search(SELF(t, side), &p);
    }
    check_buffer(t, "full_search", pass->field, COMPARE_FIELD);
}

static void tee_iterate_search(void *self, const mb_iterate_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_iterate_pass p = *pass;
        p.origin = IMAGE(pass->origin);
        p.target = IMAGE(pass->target);
        p.previous = BUFFER(pass->previous);
        p.field = BUFFER(pass->field);
        p.parent = BUFFER(pass->parent);
        p.temporal = BUFFER(pass->temporal);
        p.reverse = BUFFER(pass->reverse);
        p.history = BUFFER(pass->history);
        OPS(t, side)->iterate_search(SELF(t, side), &p);
    }
    check_buffer(t, "iterate", pass->field, COMPARE_FIELD);
}

static void tee_measure(void *self, const mb_measure_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_measure_pass p = *pass;
        p.origin = IMAGE(pass->origin);
        p.target = IMAGE(pass->target);
        p.field = BUFFER(pass->field);
        p.stats = BUFFER(pass->stats);
        OPS(t, side)->measure(SELF(t, side), &p);
    }
    check_buffer(t, "measure", pass->field, COMPARE_FIELD);
    check_buffer(t, "measure_stats", pass->stats, COMPARE_WORDS);
}

static void tee_project(void *self, const mb_project_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_project_pass p = *pass;
        p.field = BUFFER(pass->field);
        p.keys = BUFFER(pass->keys);
        OPS(t, side)->project(SELF(t, side), &p);
    }
    check_buffer(t, "project", pass->keys, COMPARE_WORDS);
}

static void tee_cross_check(void *self, const mb_cross_check_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_cross_check_pass p = *pass;
        p.origin = IMAGE(pass->origin);
        p.target = IMAGE(pass->target);
        p.field = BUFFER(pass->field);
        p.repaired = BUFFER(pass->repaired);
        p.other = BUFFER(pass->other);
        p.keys = BUFFER(pass->keys);
        OPS(t, side)->cross_check(SELF(t, side), &p);
    }
    check_buffer(t, "cross_check", pass->repaired, COMPARE_FIELD);
}

static void tee_local_search(void *self, const mb_local_search_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_local_search_pass p = *pass;
        p.origin = IMAGE(pass->origin);
        p.target = IMAGE(pass->target);
        p.field = BUFFER(pass->field);
        p.repaired = BUFFER(pass->repaired);
        p.reference = BUFFER(pass->reference);
        p.stats = BUFFER(pass->stats);
        OPS(t, side)->local_search(SELF(t, side), &p);
    }
    check_buffer(t, "local_search", pass->repaired, COMPARE_FIELD);
}

static void tee_decide_cut(void *self, const mb_cut_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_cut_pass p = *pass;
        p.stats = BUFFER(pass->stats);
        p.history = BUFFER(pass->history);
        OPS(t, side)->decide_cut(SELF(t, side), &p);
    }
    check_buffer(t, "decide_cut", pass->stats, COMPARE_WORDS);
    check_buffer(t, "cut_history", pass->history, COMPARE_FLOATS);
}

static void tee_select(void *self, const mb_select_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_select_pass p = *pass;
        p.frames = (mb_select_frames){IMAGE(pass->frames.a), IMAGE(pass->frames.b), IMAGE(pass->frames.c),
                                      IMAGE(pass->frames.z)};
        p.forward = BUFFER(pass->forward);
        p.backward = BUFFER(pass->backward);
        p.keys = BUFFER(pass->keys);
        p.selection = BUFFER(pass->selection);
        p.next_forward = BUFFER(pass->next_forward);
        p.previous_backward = BUFFER(pass->previous_backward);
        p.next_stats = BUFFER(pass->next_stats);
        p.previous_stats = BUFFER(pass->previous_stats);
        p.extra_keys = BUFFER(pass->extra_keys);
        OPS(t, side)->select(SELF(t, side), &p);
    }
    check_buffer(t, "select", pass->selection, COMPARE_FIELD);
}

static void tee_median(void *self, const mb_median_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_median_pass p = *pass;
        p.selection = BUFFER(pass->selection);
        p.filtered = BUFFER(pass->filtered);
        OPS(t, side)->median(SELF(t, side), &p);
    }
    check_buffer(t, "median", pass->filtered, COMPARE_FIELD);
}

static void tee_fallback(void *self, const mb_fallback_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_fallback_pass p = *pass;
        p.selection = BUFFER(pass->selection);
        p.weights = BUFFER(pass->weights);
        p.forward = BUFFER(pass->forward);
        p.backward = BUFFER(pass->backward);
        OPS(t, side)->fallback(SELF(t, side), &p);
    }
    check_buffer(t, "fallback", pass->weights, COMPARE_FLOATS);
}

static void tee_chroma_offsets(void *self, const mb_chroma_offset_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_chroma_offset_pass p = *pass;
        p.frames = pair_side(pass->frames, side);
        p.selection = BUFFER(pass->selection);
        p.offsets = BUFFER(pass->offsets);
        OPS(t, side)->chroma_offsets(SELF(t, side), &p);
    }
    check_buffer(t, "chroma_offsets", pass->offsets, COMPARE_FLOATS);
}

static void tee_offset_median(void *self, const mb_offset_median_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_offset_median_pass p = *pass;
        p.offsets = BUFFER(pass->offsets);
        p.filtered = BUFFER(pass->filtered);
        OPS(t, side)->offset_median(SELF(t, side), &p);
    }
    check_buffer(t, "offset_median", pass->filtered, COMPARE_FLOATS);
}

static void tee_global_motion(void *self, const mb_global_motion_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_global_motion_pass p = *pass;
        p.selection = BUFFER(pass->selection);
        p.global_motion = BUFFER(pass->global_motion);
        OPS(t, side)->global_motion(SELF(t, side), &p);
    }
    check_buffer(t, "global_motion", pass->global_motion, COMPARE_FLOATS);
}

static void tee_panels(void *self, const mb_panel_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_panel_pass p = *pass;
        p.frames = pair_side(pass->frames, side);
        p.flags = BUFFER(pass->flags);
        p.global_motion = BUFFER(pass->global_motion);
        OPS(t, side)->panels(SELF(t, side), &p);
    }
    check_buffer(t, "panels", pass->flags, COMPARE_FLOATS);
}

static void tee_panel_spread(void *self, const mb_panel_spread_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_panel_spread_pass p = *pass;
        p.flags = BUFFER(pass->flags);
        p.spread = BUFFER(pass->spread);
        p.selection = BUFFER(pass->selection);
        p.global_motion = BUFFER(pass->global_motion);
        OPS(t, side)->panel_spread(SELF(t, side), &p);
    }
    check_buffer(t, "panel_spread", pass->spread, COMPARE_FLOATS);
}

static void tee_split(void *self, const mb_split_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_split_pass p = *pass;
        p.a = IMAGE(pass->a);
        p.b = IMAGE(pass->b);
        p.selection = BUFFER(pass->selection);
        p.cells = BUFFER(pass->cells);
        p.offsets = BUFFER(pass->offsets);
        OPS(t, side)->split(SELF(t, side), &p);
    }
    check_buffer(t, "split", pass->cells, COMPARE_FIELD);
}

static void tee_interpolate(void *self, const mb_interpolate_pass *pass)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_interpolate_pass p = *pass;
        p.frames = pair_side(pass->frames, side);
        p.a_level = IMAGE(pass->a_level);
        p.b_level = IMAGE(pass->b_level);
        p.output = planes_side(pass->output, side);
        p.cells = BUFFER(pass->cells);
        p.forward = BUFFER(pass->forward);
        p.backward = BUFFER(pass->backward);
        p.stats = BUFFER(pass->stats);
        p.fallback = BUFFER(pass->fallback);
        p.offsets = BUFFER(pass->offsets);
        p.panels = BUFFER(pass->panels);
        p.global_motion = BUFFER(pass->global_motion);
        p.extrapolation = (mb_extrapolation){BUFFER(pass->extrapolation.field), BUFFER(pass->extrapolation.stats)};
        OPS(t, side)->interpolate(SELF(t, side), &p);
    }
}

const mb_backend_ops TEE_OPS = {
    .destroy = tee_destroy,
    .create_image = tee_create_image,
    .destroy_image = tee_destroy_image,
    .create_buffer = tee_create_buffer,
    .destroy_buffer = tee_destroy_buffer,
    .read_buffer = tee_read_buffer,
    .write_buffer = tee_write_buffer,
    .read_image = tee_read_image,
    .write_image = tee_write_image,
    .begin_work = tee_begin_work,
    .submit_work = tee_submit_work,
    .submit_output = tee_submit_output,
    .wait_idle = tee_wait_idle,
    .output_ready = tee_output_ready,
    .read_output = tee_read_output,
    .finish_output = tee_finish_output,
    .fill_buffer = tee_fill_buffer,
    .copy_overlap = tee_copy_overlap,
    .upload_frame = tee_upload_frame,
    .load_device_frame = tee_load_device_frame,
    .fill_from_border = tee_fill_from_border,
    .poll_bar_flags = tee_poll_bar_flags,
    .clear_bar_flags = tee_clear_bar_flags,
    .record_host_output = tee_record_host_output,
    .record_device_output = tee_record_device_output,
    .normalize = tee_normalize,
    .downsample = tee_downsample,
    .full_search = tee_full_search,
    .iterate_search = tee_iterate_search,
    .measure = tee_measure,
    .project = tee_project,
    .cross_check = tee_cross_check,
    .local_search = tee_local_search,
    .decide_cut = tee_decide_cut,
    .select = tee_select,
    .median = tee_median,
    .fallback = tee_fallback,
    .chroma_offsets = tee_chroma_offsets,
    .offset_median = tee_offset_median,
    .global_motion = tee_global_motion,
    .panels = tee_panels,
    .panel_spread = tee_panel_spread,
    .split = tee_split,
    .interpolate = tee_interpolate,
};
