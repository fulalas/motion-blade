#include "engine/framing.h"

#include "engine/crop.h"
#include "engine/geometry.h"

enum { BORDER_REFRESH_MASK = 15, DETECT_MASK = 3 };

static mb_status apply(mb_engine *engine, const bar_source *source)
{
    bar_observation observation = {source, engine->geo.active, engine->framing.push_count,
                                   engine->params.framing.settle_frames};
    bar_decision decision = bar_tracker_update(&engine->framing.tracker, &observation);
    return decision.crop ? crop_to(engine, decision.active) : MB_SUCCESS;
}

mb_status framing_observe_host(mb_engine *engine, const mb_host_image *frame)
{
    if (!engine->params.framing.crop_bars) {
        return MB_SUCCESS;
    }
    bar_source source = {&engine->source, frame, NULL};
    return apply(engine, &source);
}

mb_status framing_observe_device(mb_engine *engine)
{
    const mb_backend *b = &engine->backend;
    const uint32_t *flags = NULL;
    if (!engine->framing.flags_pending || !b->ops->poll_bar_flags(b->self, &flags)) {
        return MB_SUCCESS;
    }
    engine->framing.flags_pending = false;
    mb_status status = MB_SUCCESS;
    if (engine->params.framing.crop_bars) {
        bar_source source = {&engine->source, NULL, flags};
        status = apply(engine, &source);
    }
    b->ops->clear_bar_flags(b->self);
    return status;
}

void framing_save_border(mb_engine *engine, const mb_host_image *frame)
{
    if (geometry_is_cropped(&engine->geo, &engine->source)) {
        mb_host_image_copy_outside(&engine->framing.border.image, frame, &engine->source, engine->geo.active);
    }
}

static bool should_refresh_border(const mb_engine *engine)
{
    const framing_state *f = &engine->framing;
    bool stale = f->device_border_version != f->border_version || (f->device_ticks & BORDER_REFRESH_MASK) == 0;
    return geometry_is_cropped(&engine->geo, &engine->source) && stale;
}

static bool should_detect_bars(mb_engine *engine)
{
    if (!engine->params.framing.crop_bars || engine->framing.flags_pending) {
        return false;
    }
    return (engine->framing.device_ticks++ & DETECT_MASK) == 0;
}

mb_device_load framing_plan_device_load(mb_engine *engine, const mb_device_frame *frame, mb_frame_planes planes)
{
    bool refresh = should_refresh_border(engine);
    bool detect = should_detect_bars(engine);
    return (mb_device_load){planes, frame, &engine->source, engine->geo.active, refresh, detect};
}

void framing_note_device_load(mb_engine *engine, const mb_device_load *load)
{
    if (load->refresh_border) {
        engine->framing.device_border_version = engine->framing.border_version;
    }
    if (load->detect_bars) {
        engine->framing.flags_pending = true;
    }
}

mb_border framing_output_border(const mb_engine *engine)
{
    const framing_state *f = &engine->framing;
    return (mb_border){f->device_input ? NULL : &f->border.image, f->border_version};
}
