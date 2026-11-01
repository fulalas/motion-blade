#include "engine/crop.h"

#include <string.h>

#include "engine/geometry.h"
#include "engine/motion.h"
#include "engine/resources.h"

static mb_status finish_all_work(const mb_backend *b)
{
    mb_status status = b->ops->submit_work(b->self);
    mb_status idle = b->ops->wait_idle(b->self);
    return status != MB_SUCCESS ? status : idle;
}

mb_status allocate_geometry(mb_engine *engine)
{
    const mb_backend *b = &engine->backend;
    mb_status status = b->ops->begin_work(b->self);
    if (status == MB_SUCCESS) {
        status = frames_create(engine);
    }
    if (status == MB_SUCCESS) {
        status = buffers_create(engine);
    }
    mb_status finished = finish_all_work(b);
    return status != MB_SUCCESS ? status : finished;
}

typedef struct plane_shift_pair {
    int x;
    int y;
} plane_shift_pair;

static plane_shift_pair plane_shift(const mb_engine *engine, int plane)
{
    if (plane == 0) {
        return (plane_shift_pair){0, 0};
    }
    return (plane_shift_pair){engine->source.chroma_shift_x, engine->source.chroma_shift_y};
}

static mb_image *plane_of(const mb_frame_planes *planes, int plane)
{
    return plane == 0 ? planes->luma : planes->chroma[plane - 1];
}

static void copy_old_content(mb_engine *engine, const frame_slot *old, mb_rect old_active)
{
    const mb_backend *b = &engine->backend;
    mb_rect active = engine->geo.active;
    for (int k = 0; k < FRAME_RING; k++) {
        for (int plane = 0; plane < mb_plane_count(&engine->source); plane++) {
            plane_shift_pair s = plane_shift(engine, plane);
            mb_overlap_copy copy = {
                plane_of(&old[k].planes, plane), plane_of(&engine->frames[k].planes, plane), old_active.x >> s.x,
                old_active.y >> s.y, active.x >> s.x, active.y >> s.y,
            };
            b->ops->copy_overlap(b->self, &copy);
        }
    }
}

static void spread_first_frame(mb_engine *engine)
{
    const mb_backend *b = &engine->backend;
    for (int k = 1; k < FRAME_RING; k++) {
        for (int plane = 0; plane < mb_plane_count(&engine->source); plane++) {
            mb_image *first = plane_of(&engine->frames[0].planes, plane);
            mb_overlap_copy copy = {first, plane_of(&engine->frames[k].planes, plane), 0, 0, 0, 0};
            b->ops->copy_overlap(b->self, &copy);
        }
    }
}

static mb_status fill_with_border(mb_engine *engine, bool had_border)
{
    const mb_backend *b = &engine->backend;
    if (engine->framing.device_input) {
        for (int k = 0; k < FRAME_RING; k++) {
            b->ops->fill_from_border(b->self, engine->frames[k].planes, engine->geo.active, &engine->source);
        }
        return MB_SUCCESS;
    }
    if (!had_border) {
        return MB_SUCCESS;
    }
    mb_upload upload = {engine->frames[0].planes, &engine->framing.border.image, engine->geo.active, &engine->source};
    mb_status status = b->ops->upload_frame(b->self, &upload);
    if (status == MB_SUCCESS) {
        spread_first_frame(engine);
    }
    return status;
}

static mb_status migrate_frames(mb_engine *engine, const frame_slot *old, mb_rect old_active, bool had_border)
{
    mb_status status = engine->backend.ops->begin_work(engine->backend.self);
    if (status == MB_SUCCESS && engine->pair.frame_count > 0) {
        status = fill_with_border(engine, had_border);
    }
    if (status == MB_SUCCESS && engine->pair.frame_count > 0) {
        copy_old_content(engine, old, old_active);
        for (int k = 0; k < FRAME_RING; k++) {
            build_levels(engine, k);
        }
        if (engine->pair.frame_count >= 2) {
            status = run_motion(engine);
        }
    }
    mb_status finished = finish_all_work(&engine->backend);
    return status != MB_SUCCESS ? status : finished;
}

static void forget_pairs(mb_engine *engine)
{
    engine->pair.motion_ready = false;
    engine->pair.has_previous = false;
    engine->pair.has_older = false;
    engine->pair.history_stale = true;
}

mb_status crop_to(mb_engine *engine, mb_rect active)
{
    mb_status status = finish_all_work(&engine->backend);
    frame_slot old[FRAME_RING];
    memcpy(old, engine->frames, sizeof(old));
    memset(engine->frames, 0, sizeof(engine->frames));
    mb_rect old_active = engine->geo.active;
    buffers_destroy(engine);
    engine->geo = geometry_for(&engine->source, active, &engine->params.search);
    forget_pairs(engine);
    engine->framing.border_version++;
    bool had_border = border_exists(&engine->framing.border);
    if (status == MB_SUCCESS && !had_border && !border_create(&engine->framing.border, &engine->source)) {
        status = MB_ERROR_NO_MEMORY;
    }
    if (status == MB_SUCCESS) {
        status = allocate_geometry(engine);
    }
    if (status == MB_SUCCESS) {
        status = migrate_frames(engine, old, old_active, had_border);
    }
    frames_destroy(&engine->backend, old);
    if (status != MB_SUCCESS) {
        engine->healthy = false;
    }
    return status;
}
