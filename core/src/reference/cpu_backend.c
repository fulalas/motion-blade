#include "motionblade/cpu_backend.h"

#include <stdlib.h>
#include <string.h>

#include "reference/cpu_resources.h"
#include "reference/cpu_transfers.h"
#include "reference/kernels.h"

enum { MAX_PENDING = MB_PENDING_LIMIT, OUTPUT_SLOTS = MAX_PENDING + 1 };

typedef struct cpu_backend {
    cpu_output outputs[OUTPUT_SLOTS];
    int recorded;
    uint32_t serial;
} cpu_backend;

static void cpu_destroy(void *self)
{
    cpu_backend *cpu = self;
    for (int i = 0; i < OUTPUT_SLOTS; i++) {
        cpu_output_clear(&cpu->outputs[i]);
    }
    free(cpu);
}

static mb_status cpu_create_image(void *self, const mb_image_desc *desc, mb_image **image)
{
    (void)self;
    mb_image *created = calloc(1, sizeof(*created));
    if (!created || !ref_texture_alloc(&created->texture, desc)) {
        free(created);
        return MB_ERROR_NO_MEMORY;
    }
    *image = created;
    return MB_SUCCESS;
}

static void cpu_destroy_image(void *self, mb_image *image)
{
    (void)self;
    if (image) {
        ref_texture_free(&image->texture);
        free(image);
    }
}

static mb_status cpu_create_buffer(void *self, size_t bytes, mb_buffer **buffer)
{
    (void)self;
    mb_buffer *created = calloc(1, sizeof(*created));
    void *data = calloc(1, bytes ? bytes : 1);
    if (!created || !data) {
        free(created);
        free(data);
        return MB_ERROR_NO_MEMORY;
    }
    *created = (mb_buffer){bytes, data};
    *buffer = created;
    return MB_SUCCESS;
}

static void cpu_destroy_buffer(void *self, mb_buffer *buffer)
{
    (void)self;
    if (buffer) {
        free(buffer->data);
        free(buffer);
    }
}

static mb_status cpu_read_buffer(void *self, mb_buffer *buffer, void *data, size_t bytes)
{
    (void)self;
    memcpy(data, buffer->data, bytes < buffer->bytes ? bytes : buffer->bytes);
    return MB_SUCCESS;
}

static mb_status cpu_write_buffer(void *self, mb_buffer *buffer, const void *data, size_t bytes)
{
    (void)self;
    memcpy(buffer->data, data, bytes < buffer->bytes ? bytes : buffer->bytes);
    return MB_SUCCESS;
}

static size_t texel_count(const ref_texture *t)
{
    return mb_texel_count((mb_image_desc){t->width, t->height, t->format});
}

static mb_status cpu_read_image(void *self, mb_image *image, float *texels)
{
    (void)self;
    memcpy(texels, image->texture.texels, texel_count(&image->texture) * sizeof(float));
    return MB_SUCCESS;
}

static mb_status cpu_write_image(void *self, mb_image *image, const float *texels)
{
    (void)self;
    memcpy(image->texture.texels, texels, texel_count(&image->texture) * sizeof(float));
    return MB_SUCCESS;
}

static void cpu_prepare(void *self, const mb_video_format *format)
{
    (void)self;
    (void)format;
}

static mb_status cpu_nothing_to_do(void *self)
{
    (void)self;
    return MB_SUCCESS;
}

static cpu_output *output_of(cpu_backend *cpu, mb_ticket ticket)
{
    if (ticket.slot < 0 || ticket.slot >= OUTPUT_SLOTS) {
        return NULL;
    }
    cpu_output *output = &cpu->outputs[ticket.slot];
    return output->in_use && output->serial == ticket.serial ? output : NULL;
}

static mb_status cpu_submit_output(void *self, mb_ticket *ticket)
{
    cpu_backend *cpu = self;
    if (cpu->recorded < 0) {
        return MB_ERROR_INVALID;
    }
    cpu_output *output = &cpu->outputs[cpu->recorded];
    output->in_use = true;
    output->serial = ++cpu->serial;
    *ticket = (mb_ticket){cpu->recorded, output->serial};
    cpu->recorded = -1;
    return MB_SUCCESS;
}

static bool cpu_output_ready(void *self, mb_ticket ticket)
{
    return output_of(self, ticket) != NULL;
}

static mb_status cpu_read_output(void *self, mb_ticket ticket, const mb_host_image *target, mb_rect area)
{
    cpu_output *output = output_of(self, ticket);
    if (!output) {
        return MB_ERROR_INVALID;
    }
    cpu_download_planes(output, target, area);
    return MB_SUCCESS;
}

static mb_status cpu_finish_output(void *self, mb_ticket ticket, mb_output_stats *stats)
{
    cpu_output *output = output_of(self, ticket);
    if (!output) {
        return MB_ERROR_INVALID;
    }
    *stats = (mb_output_stats){
        .valid = true,
        .scene_cut = output->stats[MB_STAT_CUT] != 0,
        .error_sum = output->stats[MB_STAT_ERROR],
    };
    cpu_output_clear(output);
    output->in_use = false;
    return MB_SUCCESS;
}

static void cpu_fill_buffer(void *self, mb_buffer *buffer, uint32_t value)
{
    (void)self;
    uint32_t *data = buffer->data;
    for (size_t i = 0; i < buffer->bytes / sizeof(uint32_t); i++) {
        data[i] = value;
    }
}

static void cpu_copy_overlap(void *self, const mb_overlap_copy *copy)
{
    (void)self;
    const ref_texture *s = &copy->source->texture;
    ref_texture *t = &copy->target->texture;
    mb_rect source_area = {copy->source_x, copy->source_y, s->width, s->height};
    mb_rect target_area = {copy->target_x, copy->target_y, t->width, t->height};
    mb_rect overlap = mb_rect_intersection(source_area, target_area);
    for (int y = overlap.y; y < overlap.y + overlap.height; y++) {
        for (int x = overlap.x; x < overlap.x + overlap.width; x++) {
            size_t row = (size_t)(y - copy->target_y) * (size_t)t->width;
            size_t index = (row + (size_t)(x - copy->target_x)) * (size_t)t->channels;
            for (int c = 0; c < s->channels; c++) {
                t->texels[index + (size_t)c] = ref_texel(s, x - copy->source_x, y - copy->source_y, c);
            }
        }
    }
}

static mb_status cpu_upload(void *self, const mb_upload *upload)
{
    (void)self;
    cpu_upload_frame(upload);
    return MB_SUCCESS;
}

static mb_status cpu_load_device_frame(void *self, const mb_device_load *load)
{
    (void)self;
    (void)load;
    return MB_ERROR_UNSUPPORTED;
}

static void cpu_fill_from_border(void *self, mb_frame_planes planes, mb_rect area, const mb_video_format *format)
{
    (void)self;
    (void)planes;
    (void)area;
    (void)format;
}

static bool cpu_poll_bar_flags(void *self, const uint32_t **flags)
{
    (void)self;
    *flags = NULL;
    return false;
}

static void cpu_clear_bar_flags(void *self)
{
    (void)self;
}

static int free_output_slot(const cpu_backend *cpu)
{
    for (int i = 0; i < OUTPUT_SLOTS; i++) {
        if (!cpu->outputs[i].in_use) {
            return i;
        }
    }
    return -1;
}

static mb_status cpu_record_host_output(void *self, const mb_output_copy *copy, mb_buffer *stats)
{
    cpu_backend *cpu = self;
    int slot = free_output_slot(cpu);
    if (slot < 0) {
        return MB_ERROR_BUSY;
    }
    if (!cpu_output_capture(&cpu->outputs[slot], copy->planes, stats, copy->format)) {
        return MB_ERROR_NO_MEMORY;
    }
    cpu->recorded = slot;
    return MB_SUCCESS;
}

static mb_status cpu_record_device_output(void *self, const mb_output_copy *copy, mb_buffer *stats)
{
    (void)self;
    (void)copy;
    (void)stats;
    return MB_ERROR_UNSUPPORTED;
}

#define REFERENCE_PASS(name, type)                  \
    static void cpu_##name(void *self, const type *pass) \
    {                                                \
        (void)self;                                  \
        ref_##name(pass);                            \
    }

REFERENCE_PASS(normalize, mb_normalize_pass)
REFERENCE_PASS(downsample, mb_downsample_pass)
REFERENCE_PASS(full_search, mb_full_search_pass)
REFERENCE_PASS(iterate_search, mb_iterate_pass)
REFERENCE_PASS(measure, mb_measure_pass)
REFERENCE_PASS(project, mb_project_pass)
REFERENCE_PASS(cross_check, mb_cross_check_pass)
REFERENCE_PASS(local_search, mb_local_search_pass)
REFERENCE_PASS(decide_cut, mb_cut_pass)
REFERENCE_PASS(select, mb_select_pass)
REFERENCE_PASS(median, mb_median_pass)
REFERENCE_PASS(fallback, mb_fallback_pass)
REFERENCE_PASS(chroma_offsets, mb_chroma_offset_pass)
REFERENCE_PASS(offset_median, mb_offset_median_pass)
REFERENCE_PASS(global_motion, mb_global_motion_pass)
REFERENCE_PASS(panels, mb_panel_pass)
REFERENCE_PASS(panel_spread, mb_panel_spread_pass)
REFERENCE_PASS(split, mb_split_pass)
REFERENCE_PASS(interpolate, mb_interpolate_pass)
REFERENCE_PASS(extrapolate, mb_extrapolate_pass)

static const mb_backend_ops CPU_OPS = {
    .destroy = cpu_destroy,
    .prepare = cpu_prepare,
    .create_image = cpu_create_image,
    .destroy_image = cpu_destroy_image,
    .create_buffer = cpu_create_buffer,
    .destroy_buffer = cpu_destroy_buffer,
    .read_buffer = cpu_read_buffer,
    .write_buffer = cpu_write_buffer,
    .read_image = cpu_read_image,
    .write_image = cpu_write_image,
    .begin_work = cpu_nothing_to_do,
    .submit_work = cpu_nothing_to_do,
    .submit_output = cpu_submit_output,
    .wait_idle = cpu_nothing_to_do,
    .output_ready = cpu_output_ready,
    .read_output = cpu_read_output,
    .finish_output = cpu_finish_output,
    .fill_buffer = cpu_fill_buffer,
    .copy_overlap = cpu_copy_overlap,
    .upload_frame = cpu_upload,
    .load_device_frame = cpu_load_device_frame,
    .fill_from_border = cpu_fill_from_border,
    .poll_bar_flags = cpu_poll_bar_flags,
    .clear_bar_flags = cpu_clear_bar_flags,
    .record_host_output = cpu_record_host_output,
    .record_device_output = cpu_record_device_output,
    .normalize = cpu_normalize,
    .downsample = cpu_downsample,
    .full_search = cpu_full_search,
    .iterate_search = cpu_iterate_search,
    .measure = cpu_measure,
    .project = cpu_project,
    .cross_check = cpu_cross_check,
    .local_search = cpu_local_search,
    .decide_cut = cpu_decide_cut,
    .select = cpu_select,
    .median = cpu_median,
    .fallback = cpu_fallback,
    .chroma_offsets = cpu_chroma_offsets,
    .offset_median = cpu_offset_median,
    .global_motion = cpu_global_motion,
    .panels = cpu_panels,
    .panel_spread = cpu_panel_spread,
    .split = cpu_split,
    .interpolate = cpu_interpolate,
    .extrapolate = cpu_extrapolate,
};

mb_status mb_cpu_backend_create(mb_backend *backend)
{
    cpu_backend *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) {
        return MB_ERROR_NO_MEMORY;
    }
    cpu->recorded = -1;
    *backend = (mb_backend){&CPU_OPS, cpu, MAX_PENDING, 0};
    return MB_SUCCESS;
}
