#include "engine/resources.h"

#include <string.h>

#include "engine/geometry.h"

enum { FIELD_BYTES = 16, KEY_BYTES = 16, CELL_BYTES = 64, FLOAT_BYTES = 4, FLAG_BYTES = 8 };
enum { STATS_BYTES = MB_STAT_COUNT * 4, HISTORY_BYTES = MB_HIST_COUNT * 4, GLOBAL_BYTES = 16 };

typedef struct allocator {
    const mb_backend *backend;
    mb_status status;
} allocator;

static mb_buffer *new_buffer(allocator *a, size_t bytes)
{
    mb_buffer *buffer = NULL;
    if (a->status == MB_SUCCESS) {
        a->status = a->backend->ops->create_buffer(a->backend->self, bytes, &buffer);
    }
    return a->status == MB_SUCCESS ? buffer : NULL;
}

static mb_image *new_image(allocator *a, int width, int height, mb_texel_format format)
{
    mb_image *image = NULL;
    mb_image_desc desc = {width, height, format};
    if (a->status == MB_SUCCESS) {
        a->status = a->backend->ops->create_image(a->backend->self, &desc, &image);
    }
    return a->status == MB_SUCCESS ? image : NULL;
}

static void free_buffer(const mb_backend *backend, mb_buffer **buffer)
{
    if (*buffer) {
        backend->ops->destroy_buffer(backend->self, *buffer);
        *buffer = NULL;
    }
}

static void free_image(const mb_backend *backend, mb_image **image)
{
    if (*image) {
        backend->ops->destroy_image(backend->self, *image);
        *image = NULL;
    }
}

static mb_frame_planes new_planes(allocator *a, const geometry *g)
{
    mb_frame_planes planes = {0};
    planes.luma = new_image(a, g->format.width, g->format.height, geometry_luma_texels(g));
    planes.chroma[0] = new_image(a, g->chroma.width, g->chroma.height, geometry_chroma_texels(g));
    if (!g->chroma.interleaved) {
        planes.chroma[1] = new_image(a, g->chroma.width, g->chroma.height, geometry_chroma_texels(g));
    }
    return planes;
}

static void free_planes(const mb_backend *backend, mb_frame_planes *planes)
{
    free_image(backend, &planes->luma);
    free_image(backend, &planes->chroma[0]);
    free_image(backend, &planes->chroma[1]);
}

mb_status frames_create(mb_engine *engine)
{
    allocator a = {&engine->backend, MB_SUCCESS};
    const geometry *g = &engine->geo;
    for (int k = 0; k < FRAME_RING; k++) {
        frame_slot *slot = &engine->frames[k];
        slot->planes = new_planes(&a, g);
        for (int level = g->normalized_luma ? 0 : 1; level < g->level_count; level++) {
            mb_grid grid = g->grids[level];
            slot->levels[level] = new_image(&a, grid.width, grid.height, MB_TEXELS_R16F);
        }
    }
    return a.status;
}

void frames_destroy(const mb_backend *backend, frame_slot frames[FRAME_RING])
{
    for (int k = 0; k < FRAME_RING; k++) {
        free_planes(backend, &frames[k].planes);
        for (int level = 0; level < MB_MAX_LEVELS; level++) {
            free_image(backend, &frames[k].levels[level]);
        }
    }
}

static size_t field_bytes(const geometry *g, int level)
{
    return (size_t)mb_grid_block_count(g->grids[level]) * FIELD_BYTES;
}

static void create_pair(allocator *a, pair_motion *m, const geometry *g)
{
    for (int level = g->base_level; level < g->level_count; level++) {
        m->forward[level] = new_buffer(a, field_bytes(g, level));
        m->backward[level] = new_buffer(a, field_bytes(g, level));
    }
    m->stats = new_buffer(a, STATS_BYTES);
    m->ready = false;
}

static void create_fields(allocator *a, motion_fields *f, const geometry *g)
{
    for (int k = 0; k < PAIR_RING; k++) {
        create_pair(a, &f->pairs[k], g);
    }
    for (int level = g->base_level; level < g->level_count; level++) {
        f->scratch[level] = new_buffer(a, field_bytes(g, level));
    }
    size_t blocks = (size_t)geometry_base_blocks(g);
    f->history = new_buffer(a, HISTORY_BYTES);
    f->cross_keys = new_buffer(a, blocks * KEY_BYTES);
}

static void create_selection(allocator *a, selection_buffers *s, const geometry *g)
{
    size_t blocks = (size_t)geometry_base_blocks(g);
    s->keys = new_buffer(a, blocks * KEY_BYTES);
    s->extra_keys = new_buffer(a, blocks * KEY_BYTES);
    s->selection[0] = new_buffer(a, blocks * FIELD_BYTES);
    s->selection[1] = new_buffer(a, blocks * FIELD_BYTES);
    s->cells = new_buffer(a, blocks * CELL_BYTES);
    s->fallback = new_buffer(a, blocks * FLOAT_BYTES);
    s->offsets = new_buffer(a, blocks * FIELD_BYTES);
    s->offsets_median = new_buffer(a, blocks * FIELD_BYTES);
    s->global_motion = new_buffer(a, GLOBAL_BYTES);
    s->panel_flags = new_buffer(a, blocks * FLAG_BYTES);
    s->panel_spread = new_buffer(a, blocks * FLOAT_BYTES);
}

mb_status buffers_create(mb_engine *engine)
{
    allocator a = {&engine->backend, MB_SUCCESS};
    create_fields(&a, &engine->fields, &engine->geo);
    create_selection(&a, &engine->sel, &engine->geo);
    engine->output = new_planes(&a, &engine->geo);
    return a.status;
}

static void destroy_pair(const mb_backend *backend, pair_motion *m)
{
    for (int level = 0; level < MB_MAX_LEVELS; level++) {
        free_buffer(backend, &m->forward[level]);
        free_buffer(backend, &m->backward[level]);
    }
    free_buffer(backend, &m->stats);
    m->ready = false;
}

static void destroy_fields(const mb_backend *backend, motion_fields *f)
{
    for (int k = 0; k < PAIR_RING; k++) {
        destroy_pair(backend, &f->pairs[k]);
    }
    for (int level = 0; level < MB_MAX_LEVELS; level++) {
        free_buffer(backend, &f->scratch[level]);
    }
    free_buffer(backend, &f->history);
    free_buffer(backend, &f->cross_keys);
}

static void destroy_selection(const mb_backend *backend, selection_buffers *s)
{
    mb_buffer **all[] = {
        &s->keys, &s->extra_keys, &s->selection[0], &s->selection[1], &s->cells, &s->fallback,
        &s->offsets, &s->offsets_median, &s->global_motion, &s->panel_flags, &s->panel_spread,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        free_buffer(backend, all[i]);
    }
}

void buffers_destroy(mb_engine *engine)
{
    destroy_fields(&engine->backend, &engine->fields);
    destroy_selection(&engine->backend, &engine->sel);
    free_planes(&engine->backend, &engine->output);
    memset(&engine->debug, 0, sizeof(engine->debug));
}
