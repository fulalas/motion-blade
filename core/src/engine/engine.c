#include <stdlib.h>
#include <string.h>

#include "engine/crop.h"
#include "engine/framing.h"
#include "engine/geometry.h"
#include "engine/interpolation.h"
#include "engine/motion.h"
#include "engine/outputs.h"
#include "engine/resources.h"

static mb_status fail(mb_engine *engine, mb_status status)
{
    if (status != MB_SUCCESS && status != MB_ERROR_BUSY && status != MB_ERROR_INVALID) {
        engine->healthy = false;
    }
    return status;
}

static void release_all(mb_engine *engine)
{
    const mb_backend *b = &engine->backend;
    b->ops->submit_work(b->self);
    outputs_discard_all(engine);
    b->ops->wait_idle(b->self);
}

void mb_engine_destroy(mb_engine *engine)
{
    if (!engine) {
        return;
    }
    if (engine->backend.ops) {
        release_all(engine);
        frames_destroy(&engine->backend, engine->frames);
        buffers_destroy(engine);
        engine->backend.ops->destroy(engine->backend.self);
    }
    border_destroy(&engine->framing.border);
    free(engine);
}

static bool config_is_valid(const mb_engine_config *config)
{
    return mb_video_format_is_valid(&config->format) && mb_backend_is_complete(&config->backend);
}

static void destroy_backend(const mb_backend *backend)
{
    if (backend->ops && backend->ops->destroy) {
        backend->ops->destroy(backend->self);
    }
}

static mb_status check_size(const mb_engine *engine)
{
    long blocks = geometry_base_blocks(&engine->geo);
    if (engine->backend.max_blocks > 0 && blocks > engine->backend.max_blocks) {
        mb_log(&engine->logger, MB_LOG_WARNING, "%dx%d is too large for this GPU", engine->source.width,
               engine->source.height);
        return MB_ERROR_UNSUPPORTED;
    }
    return MB_SUCCESS;
}

static mb_engine *new_engine(const mb_engine_config *config)
{
    mb_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        return NULL;
    }
    engine->backend = config->backend;
    engine->logger = config->logger;
    engine->params = config->params;
    engine->source = config->format;
    engine->pair.history_stale = true;
    engine->geo = geometry_for(&engine->source, mb_full_rect(&engine->source), &engine->params.search);
    return engine;
}

mb_status mb_engine_create(const mb_engine_config *config, mb_engine **out)
{
    if (!config_is_valid(config)) {
        destroy_backend(&config->backend);
        return MB_ERROR_INVALID;
    }
    mb_engine *engine = new_engine(config);
    if (!engine) {
        destroy_backend(&config->backend);
        return MB_ERROR_NO_MEMORY;
    }
    mb_status status = check_size(engine);
    if (status == MB_SUCCESS) {
        status = allocate_geometry(engine);
    }
    if (status != MB_SUCCESS) {
        mb_engine_destroy(engine);
        return status;
    }
    engine->backend.ops->prepare(engine->backend.self, &engine->source);
    engine->healthy = true;
    *out = engine;
    return MB_SUCCESS;
}

bool mb_engine_is_healthy(const mb_engine *engine)
{
    return engine && engine->healthy;
}

mb_status mb_engine_set_param(mb_engine *engine, const char *name, double value)
{
    int base = engine->geo.base_level;
    mb_status status = mb_params_set(&engine->params, name, value);
    if (status != MB_SUCCESS || strcmp(name, "half_resolution") != 0) {
        return status;
    }
    geometry next = geometry_for(&engine->source, engine->geo.active, &engine->params.search);
    return engine->healthy && next.base_level != base ? crop_to(engine, engine->geo.active) : MB_SUCCESS;
}

static int next_slot(const mb_engine *engine)
{
    return (engine->pair.newest + 1) % FRAME_RING;
}

static void advance_pairs(mb_engine *engine)
{
    engine->pair.end_of_stream = false;
    engine->framing.push_count++;
    if (engine->pair.frame_count < 2 || !pair_at(engine, 0)->ready) {
        motion_forget(engine);
    }
    motion_advance(engine);
}

static mb_status commit_frame(mb_engine *engine)
{
    const mb_backend *b = &engine->backend;
    advance_pairs(engine);
    mb_status status = b->ops->submit_work(b->self);
    if (status == MB_SUCCESS) {
        status = b->ops->begin_work(b->self);
    }
    if (status != MB_SUCCESS) {
        return status;
    }
    engine->pair.newest = next_slot(engine);
    engine->frames[engine->pair.newest].border_version = engine->framing.border_version;
    engine->frames[engine->pair.newest].id = ++engine->pair.pushed;
    build_levels(engine, engine->pair.newest);
    if (engine->pair.frame_count < FRAME_RING) {
        engine->pair.frame_count++;
    }
    if (engine->pair.frame_count >= 2) {
        status = run_motion(engine);
    }
    return status == MB_SUCCESS ? b->ops->submit_work(b->self) : status;
}

static mb_status stage_host_frame(mb_engine *engine, const mb_host_image *frame)
{
    const mb_backend *b = &engine->backend;
    mb_status status = b->ops->begin_work(b->self);
    if (status != MB_SUCCESS) {
        return status;
    }
    mb_upload upload = {engine->frames[next_slot(engine)].planes, frame, engine->geo.active, &engine->source};
    return b->ops->upload_frame(b->self, &upload);
}

mb_status mb_engine_push(mb_engine *engine, const mb_host_image *frame)
{
    if (!engine->healthy) {
        return MB_ERROR_DEVICE;
    }
    engine->framing.device_input = false;
    mb_status status = framing_observe_host(engine, frame);
    if (status != MB_SUCCESS) {
        return fail(engine, status);
    }
    framing_save_border(engine, frame);
    status = stage_host_frame(engine, frame);
    if (status == MB_SUCCESS) {
        status = commit_frame(engine);
    }
    return fail(engine, status);
}

static mb_status stage_device_frame(mb_engine *engine, const mb_device_frame *frame)
{
    const mb_backend *b = &engine->backend;
    mb_status status = b->ops->begin_work(b->self);
    if (status != MB_SUCCESS) {
        return status;
    }
    mb_device_load load = framing_plan_device_load(engine, frame, engine->frames[next_slot(engine)].planes);
    status = b->ops->load_device_frame(b->self, &load);
    if (status == MB_SUCCESS) {
        framing_note_device_load(engine, &load);
    }
    return status;
}

mb_status mb_engine_push_device(mb_engine *engine, const mb_device_frame *frame)
{
    if (!engine->healthy) {
        return MB_ERROR_DEVICE;
    }
    engine->framing.device_input = true;
    mb_status status = framing_observe_device(engine);
    if (status != MB_SUCCESS) {
        return fail(engine, status);
    }
    status = stage_device_frame(engine, frame);
    if (status != MB_SUCCESS) {
        return status == MB_ERROR_UNSUPPORTED ? status : fail(engine, status);
    }
    return fail(engine, commit_frame(engine));
}

void mb_engine_end_stream(mb_engine *engine)
{
    engine->pair.end_of_stream = true;
}

void mb_engine_reset(mb_engine *engine)
{
    release_all(engine);
    engine->pair = (pair_state){.pushed = engine->pair.pushed, .newest = engine->pair.newest, .history_stale = true};
    motion_forget(engine);
}

static bool lookahead_ready(const mb_engine *engine)
{
    return !engine->pair.end_of_stream && pair_at(engine, 1)->ready && pair_at(engine, 0)->ready;
}

static mb_status check_submit(const mb_engine *engine)
{
    if (!engine->healthy) {
        return MB_ERROR_DEVICE;
    }
    if (engine->pair.frame_count < 2 || outputs_pending(engine) >= engine->backend.max_pending) {
        return MB_ERROR_BUSY;
    }
    return !engine->pair.end_of_stream && !lookahead_ready(engine) ? MB_ERROR_BUSY : MB_SUCCESS;
}

static mb_status record_pair(mb_engine *engine, double t, pair_choice *pair)
{
    const mb_backend *b = &engine->backend;
    int end = lookahead_ready(engine) ? 1 : 0;
    mb_status status = b->ops->begin_work(b->self);
    if (status == MB_SUCCESS && !pair_at(engine, 0)->ready) {
        status = run_motion(engine);
    }
    if (status == MB_SUCCESS) {
        *pair = choose_pair(engine, end);
        record_interpolation(engine, pair, t);
    }
    return status;
}

static mb_output_copy output_copy(const mb_engine *engine, const mb_device_frame *target)
{
    return (mb_output_copy){engine->output, engine->geo.active, &engine->source, target, framing_output_border(engine)};
}

static mb_status submit(mb_engine *engine, double t, const mb_device_frame *target)
{
    mb_status status = check_submit(engine);
    pair_choice pair;
    if (status == MB_SUCCESS) {
        status = record_pair(engine, t, &pair);
    }
    if (status != MB_SUCCESS) {
        return fail(engine, status);
    }
    const mb_backend *b = &engine->backend;
    mb_output_copy copy = output_copy(engine, target);
    status = target ? b->ops->record_device_output(b->self, &copy, pair.current->stats)
                    : b->ops->record_host_output(b->self, &copy, pair.current->stats);
    if (status == MB_SUCCESS) {
        status = outputs_submit(engine, target ? OUTPUT_DEVICE : OUTPUT_HOST);
    }
    return status == MB_ERROR_UNSUPPORTED ? status : fail(engine, status);
}

mb_status mb_engine_submit(mb_engine *engine, double t)
{
    return submit(engine, t, NULL);
}

mb_status mb_engine_submit_device(mb_engine *engine, double t, const mb_device_frame *target)
{
    return submit(engine, t, target);
}

uint64_t mb_engine_newest_frame(const mb_engine *engine)
{
    return engine->pair.pushed;
}

static const frame_slot *slot_with_id(const mb_engine *engine, uint64_t id)
{
    for (int back = 0; back < engine->pair.frame_count; back++) {
        const frame_slot *slot = &engine->frames[ring_index(engine, back)];
        if (slot->id == id) {
            return slot->border_version == engine->framing.border_version ? slot : NULL;
        }
    }
    return NULL;
}

static mb_status check_real(const mb_engine *engine, const frame_slot *slot)
{
    if (!engine->healthy) {
        return MB_ERROR_DEVICE;
    }
    if (!slot) {
        return MB_ERROR_UNSUPPORTED;
    }
    return outputs_pending(engine) >= engine->backend.max_pending ? MB_ERROR_BUSY : MB_SUCCESS;
}

mb_status mb_engine_submit_real(mb_engine *engine, uint64_t id, const mb_host_image *frame,
                                const mb_device_frame *target)
{
    const frame_slot *slot = slot_with_id(engine, id);
    mb_status status = check_real(engine, slot);
    const mb_backend *b = &engine->backend;
    if (status == MB_SUCCESS) {
        status = b->ops->begin_work(b->self);
    }
    if (status == MB_SUCCESS) {
        mb_border border = {frame, MB_BORDER_FROM_FRAME};
        mb_output_copy copy = {slot->planes, engine->geo.active, &engine->source, target, border};
        status = b->ops->record_device_output(b->self, &copy, pair_at(engine, 0)->stats);
    }
    if (status == MB_SUCCESS) {
        status = outputs_submit(engine, OUTPUT_DEVICE);
    }
    return status == MB_ERROR_UNSUPPORTED || status == MB_ERROR_BUSY ? status : fail(engine, status);
}

int mb_engine_pending(const mb_engine *engine)
{
    return outputs_pending(engine);
}

int mb_engine_capacity(const mb_engine *engine)
{
    return engine->backend.max_pending;
}

bool mb_engine_output_ready(const mb_engine *engine)
{
    return outputs_head_ready(engine);
}

mb_status mb_engine_retrieve(mb_engine *engine, const mb_host_image *target, mb_frame_report *report)
{
    return fail(engine, outputs_retrieve_host(engine, target, report));
}

mb_status mb_engine_finish_device(mb_engine *engine, mb_frame_report *report)
{
    return fail(engine, outputs_finish_device(engine, report));
}

static mb_buffer *debug_buffer(const mb_engine *engine, mb_field_kind kind)
{
    switch (kind) {
    case MB_FIELD_FORWARD:
        return engine->debug.forward;
    case MB_FIELD_BACKWARD:
        return engine->debug.backward;
    case MB_FIELD_SELECTION:
        return engine->debug.selection;
    }
    return NULL;
}

mb_status mb_engine_read_field(mb_engine *engine, mb_field_kind kind, mb_field_dump *dump)
{
    mb_buffer *buffer = debug_buffer(engine, kind);
    if (!buffer) {
        return MB_ERROR_INVALID;
    }
    mb_grid grid = geometry_base_grid(&engine->geo);
    dump->blocks_x = grid.blocks_x;
    dump->blocks_y = grid.blocks_y;
    size_t bytes = (size_t)mb_grid_block_count(grid) * 4 * sizeof(float);
    return engine->backend.ops->read_buffer(engine->backend.self, buffer, dump->vectors, bytes);
}
