#include "engine/outputs.h"

#include <string.h>

#include "engine/geometry.h"

mb_status outputs_submit(mb_engine *engine, output_kind kind)
{
    output_queue *q = &engine->queue;
    if (q->count >= MAX_QUEUE) {
        return MB_ERROR_BUSY;
    }
    mb_ticket ticket;
    mb_status status = engine->backend.ops->submit_output(engine->backend.self, &ticket);
    if (status == MB_SUCCESS) {
        q->entries[q->count++] = (queued_output){ticket, kind, engine->geo.active, geometry_base_blocks(&engine->geo)};
    }
    return status;
}

int outputs_pending(const mb_engine *engine)
{
    return engine->queue.count;
}

bool outputs_head_ready(const mb_engine *engine)
{
    const output_queue *q = &engine->queue;
    return q->count > 0 && engine->backend.ops->output_ready(engine->backend.self, q->entries[0].ticket);
}

static void pop_head(output_queue *q)
{
    memmove(&q->entries[0], &q->entries[1], (size_t)(q->count - 1) * sizeof(q->entries[0]));
    q->count--;
}

static mb_frame_report report_of(const mb_output_stats *stats, int block_count)
{
    double mean = (double)stats->error_sum * MB_STAT_SCALE / ((double)block_count * 256);
    return (mb_frame_report){stats->valid, stats->scene_cut, stats->valid ? mean : 0.0, stats->gpu_ms};
}

static mb_status finish_head(mb_engine *engine, mb_frame_report *report)
{
    output_queue *q = &engine->queue;
    mb_output_stats stats = {0};
    mb_status status = engine->backend.ops->finish_output(engine->backend.self, q->entries[0].ticket, &stats);
    *report = report_of(&stats, q->entries[0].block_count);
    pop_head(q);
    return status;
}

static bool is_full_frame(const mb_engine *engine, mb_rect area)
{
    return mb_rect_equal(area, (mb_rect){0, 0, engine->source.width, engine->source.height});
}

mb_status outputs_retrieve_host(mb_engine *engine, const mb_host_image *target, mb_frame_report *report)
{
    output_queue *q = &engine->queue;
    if (q->count == 0 || q->entries[0].kind != OUTPUT_HOST) {
        return MB_ERROR_INVALID;
    }
    mb_rect area = q->entries[0].area;
    mb_status status = engine->backend.ops->read_output(engine->backend.self, q->entries[0].ticket, target, area);
    if (status == MB_SUCCESS && !is_full_frame(engine, area)) {
        mb_host_image_copy_outside(target, &engine->framing.border.image, &engine->source, area);
    }
    mb_status finished = finish_head(engine, report);
    return status != MB_SUCCESS ? status : finished;
}

mb_status outputs_finish_device(mb_engine *engine, mb_frame_report *report)
{
    output_queue *q = &engine->queue;
    if (q->count == 0 || q->entries[0].kind != OUTPUT_DEVICE) {
        return MB_ERROR_INVALID;
    }
    return finish_head(engine, report);
}

void outputs_discard_all(mb_engine *engine)
{
    mb_frame_report ignored;
    while (engine->queue.count > 0) {
        finish_head(engine, &ignored);
    }
}
