#include "tee_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float FLOAT_TOLERANCE = 1e-3f;
static const float LEVEL_TOLERANCE = 0.5f;

void *image_side(const mb_image *image, int side)
{
    return image ? image->side[side] : NULL;
}

void *buffer_side(const mb_buffer *buffer, int side)
{
    return buffer ? buffer->side[side] : NULL;
}

const tee_pass_report *tee_find(const tee_report *report, const char *name)
{
    for (int i = 0; i < report->count; i++) {
        if (!strcmp(report->passes[i].name, name)) {
            return &report->passes[i];
        }
    }
    return NULL;
}

static tee_pass_report *entry(tee *t, const char *name)
{
    tee_pass_report *found = (tee_pass_report *)tee_find(t->report, name);
    if (found || t->report->count == TEE_MAX_PASSES) {
        return found;
    }
    found = &t->report->passes[t->report->count++];
    *found = (tee_pass_report){name, 0, 1.0, 0.0};
    return found;
}

static void note(tee *t, const char *name, double agreement, double difference)
{
    tee_pass_report *r = entry(t, name);
    if (r) {
        r->runs++;
        r->worst_agreement = fmin(r->worst_agreement, agreement);
        r->worst_difference = fmax(r->worst_difference, difference);
    }
}

typedef struct comparison {
    double agreement;
    double difference;
} comparison;

static comparison compare_fields(const float *a, const float *b, size_t count)
{
    size_t same = 0;
    double difference = 0;
    for (size_t i = 0; i < count; i += 4) {
        bool equal = a[i] == b[i] && a[i + 1] == b[i + 1];
        same += equal;
        for (size_t k = 0; equal && k < 4; k++) {
            difference = fmax(difference, fabs((double)a[i + k] - b[i + k]));
        }
    }
    return (comparison){count ? (double)same / (double)(count / 4) : 1.0, difference};
}

static comparison compare_words(const uint32_t *a, const uint32_t *b, size_t count)
{
    size_t same = 0;
    for (size_t i = 0; i < count; i++) {
        same += a[i] == b[i];
    }
    return (comparison){count ? (double)same / (double)count : 1.0, 0.0};
}

static comparison compare_floats(const float *a, const float *b, size_t count, float tolerance)
{
    size_t same = 0;
    double difference = 0;
    for (size_t i = 0; i < count; i++) {
        double d = fabs((double)a[i] - b[i]);
        same += d <= tolerance;
        difference = fmax(difference, d);
    }
    return (comparison){count ? (double)same / (double)count : 1.0, difference};
}

static comparison compare_data(compare_kind kind, const void *a, const void *b, size_t bytes)
{
    switch (kind) {
    case COMPARE_FIELD:
        return compare_fields(a, b, bytes / sizeof(float));
    case COMPARE_WORDS:
        return compare_words(a, b, bytes / sizeof(uint32_t));
    case COMPARE_FLOATS:
        return compare_floats(a, b, bytes / sizeof(float), FLOAT_TOLERANCE);
    }
    return (comparison){1.0, 0.0};
}

void check_buffer(tee *t, const char *name, mb_buffer *buffer, compare_kind kind)
{
    void *a = malloc(buffer->bytes);
    void *b = malloc(buffer->bytes);
    bool read = a && b &&
                OPS(t, REFERENCE)->read_buffer(SELF(t, REFERENCE), buffer->side[REFERENCE], a, buffer->bytes) ==
                    MB_SUCCESS &&
                OPS(t, DEVICE)->read_buffer(SELF(t, DEVICE), buffer->side[DEVICE], b, buffer->bytes) == MB_SUCCESS;
    if (read) {
        comparison c = compare_data(kind, a, b, buffer->bytes);
        note(t, name, c.agreement, c.difference);
        OPS(t, DEVICE)->write_buffer(SELF(t, DEVICE), buffer->side[DEVICE], a, buffer->bytes);
    }
    OPS(t, DEVICE)->begin_work(SELF(t, DEVICE));
    free(a);
    free(b);
}

static size_t image_values(const mb_image *image)
{
    return (size_t)image->width * (size_t)image->height * (size_t)mb_texel_channels(image->format);
}

void check_image(tee *t, const char *name, mb_image *image)
{
    size_t count = image_values(image);
    float *a = malloc(count * sizeof(float));
    float *b = malloc(count * sizeof(float));
    if (a && b && OPS(t, REFERENCE)->read_image(SELF(t, REFERENCE), image->side[REFERENCE], a) == MB_SUCCESS &&
        OPS(t, DEVICE)->read_image(SELF(t, DEVICE), image->side[DEVICE], b) == MB_SUCCESS) {
        comparison c = compare_floats(a, b, count, LEVEL_TOLERANCE / 255.0f);
        note(t, name, c.agreement, c.difference * 255.0);
        OPS(t, DEVICE)->write_image(SELF(t, DEVICE), image->side[DEVICE], a);
    }
    OPS(t, DEVICE)->begin_work(SELF(t, DEVICE));
    free(a);
    free(b);
}

void tee_destroy(void *self)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        OPS(t, side)->destroy(SELF(t, side));
    }
    free(t);
}

mb_status tee_create_image(void *self, const mb_image_desc *desc, mb_image **out)
{
    tee *t = self;
    mb_image *image = calloc(1, sizeof(*image));
    if (!image) {
        return MB_ERROR_NO_MEMORY;
    }
    *image = (mb_image){{NULL, NULL}, desc->width, desc->height, desc->format};
    for (int side = 0; side < SIDES; side++) {
        mb_status status = OPS(t, side)->create_image(SELF(t, side), desc, (mb_image **)&image->side[side]);
        if (status != MB_SUCCESS) {
            return status;
        }
    }
    *out = image;
    return MB_SUCCESS;
}

void tee_destroy_image(void *self, mb_image *image)
{
    tee *t = self;
    for (int side = 0; side < SIDES && image; side++) {
        OPS(t, side)->destroy_image(SELF(t, side), image->side[side]);
    }
    free(image);
}

mb_status tee_create_buffer(void *self, size_t bytes, mb_buffer **out)
{
    tee *t = self;
    mb_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return MB_ERROR_NO_MEMORY;
    }
    buffer->bytes = bytes;
    for (int side = 0; side < SIDES; side++) {
        mb_status status = OPS(t, side)->create_buffer(SELF(t, side), bytes, (mb_buffer **)&buffer->side[side]);
        if (status != MB_SUCCESS) {
            return status;
        }
    }
    *out = buffer;
    return MB_SUCCESS;
}

void tee_destroy_buffer(void *self, mb_buffer *buffer)
{
    tee *t = self;
    for (int side = 0; side < SIDES && buffer; side++) {
        OPS(t, side)->destroy_buffer(SELF(t, side), buffer->side[side]);
    }
    free(buffer);
}

mb_status tee_read_buffer(void *self, mb_buffer *buffer, void *data, size_t bytes)
{
    tee *t = self;
    return OPS(t, REFERENCE)->read_buffer(SELF(t, REFERENCE), buffer->side[REFERENCE], data, bytes);
}

mb_status tee_write_buffer(void *self, mb_buffer *buffer, const void *data, size_t bytes)
{
    tee *t = self;
    mb_status status = MB_SUCCESS;
    for (int side = 0; side < SIDES && status == MB_SUCCESS; side++) {
        status = OPS(t, side)->write_buffer(SELF(t, side), buffer->side[side], data, bytes);
    }
    return status;
}

mb_status tee_read_image(void *self, mb_image *image, float *texels)
{
    tee *t = self;
    return OPS(t, REFERENCE)->read_image(SELF(t, REFERENCE), image->side[REFERENCE], texels);
}

mb_status tee_write_image(void *self, mb_image *image, const float *texels)
{
    tee *t = self;
    mb_status status = MB_SUCCESS;
    for (int side = 0; side < SIDES && status == MB_SUCCESS; side++) {
        status = OPS(t, side)->write_image(SELF(t, side), image->side[side], texels);
    }
    return status;
}

static mb_status both(tee *t, mb_status (*call)(const mb_backend *backend))
{
    mb_status status = MB_SUCCESS;
    for (int side = 0; side < SIDES && status == MB_SUCCESS; side++) {
        status = call(&t->backends[side]);
    }
    return status;
}

static mb_status begin(const mb_backend *b)
{
    return b->ops->begin_work(b->self);
}

static mb_status submit(const mb_backend *b)
{
    return b->ops->submit_work(b->self);
}

static mb_status idle(const mb_backend *b)
{
    return b->ops->wait_idle(b->self);
}

mb_status tee_begin_work(void *self)
{
    return both(self, begin);
}

mb_status tee_submit_work(void *self)
{
    return both(self, submit);
}

mb_status tee_wait_idle(void *self)
{
    return both(self, idle);
}

mb_status tee_submit_output(void *self, mb_ticket *ticket)
{
    tee *t = self;
    mb_status status = OPS(t, REFERENCE)->submit_output(SELF(t, REFERENCE), ticket);
    mb_ticket device;
    if (status == MB_SUCCESS) {
        status = OPS(t, DEVICE)->submit_output(SELF(t, DEVICE), &device);
    }
    if (status == MB_SUCCESS) {
        t->device_tickets[ticket->slot % TICKETS] = device;
    }
    return status;
}

bool tee_output_ready(void *self, mb_ticket ticket)
{
    tee *t = self;
    return OPS(t, REFERENCE)->output_ready(SELF(t, REFERENCE), ticket);
}

mb_status tee_read_output(void *self, mb_ticket ticket, const mb_host_image *target, mb_rect area)
{
    tee *t = self;
    return OPS(t, REFERENCE)->read_output(SELF(t, REFERENCE), ticket, target, area);
}

mb_status tee_finish_output(void *self, mb_ticket ticket, mb_output_stats *stats)
{
    tee *t = self;
    mb_output_stats device;
    mb_status status = OPS(t, REFERENCE)->finish_output(SELF(t, REFERENCE), ticket, stats);
    OPS(t, DEVICE)->finish_output(SELF(t, DEVICE), t->device_tickets[ticket.slot % TICKETS], &device);
    note(t, "output_cut", stats->scene_cut == device.scene_cut ? 1.0 : 0.0, 0.0);
    return status;
}

void tee_fill_buffer(void *self, mb_buffer *buffer, uint32_t value)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        OPS(t, side)->fill_buffer(SELF(t, side), BUFFER(buffer), value);
    }
}

void tee_copy_overlap(void *self, const mb_overlap_copy *copy)
{
    tee *t = self;
    for (int side = 0; side < SIDES; side++) {
        mb_overlap_copy c = *copy;
        c.source = IMAGE(copy->source);
        c.target = IMAGE(copy->target);
        OPS(t, side)->copy_overlap(SELF(t, side), &c);
    }
}

mb_frame_planes planes_side(mb_frame_planes planes, int side)
{
    return (mb_frame_planes){IMAGE(planes.luma), {IMAGE(planes.chroma[0]), IMAGE(planes.chroma[1])}};
}

mb_status tee_upload_frame(void *self, const mb_upload *upload)
{
    tee *t = self;
    mb_status status = MB_SUCCESS;
    for (int side = 0; side < SIDES && status == MB_SUCCESS; side++) {
        mb_upload u = *upload;
        u.planes = planes_side(upload->planes, side);
        status = OPS(t, side)->upload_frame(SELF(t, side), &u);
    }
    return status;
}

mb_status tee_load_device_frame(void *self, const mb_device_load *load)
{
    (void)self;
    (void)load;
    return MB_ERROR_UNSUPPORTED;
}

void tee_fill_from_border(void *self, mb_frame_planes planes, mb_rect area, const mb_video_format *format)
{
    (void)self;
    (void)planes;
    (void)area;
    (void)format;
}

bool tee_poll_bar_flags(void *self, const uint32_t **flags)
{
    (void)self;
    *flags = NULL;
    return false;
}

void tee_clear_bar_flags(void *self)
{
    (void)self;
}

mb_status tee_record_host_output(void *self, const mb_output_copy *copy, mb_buffer *stats)
{
    tee *t = self;
    check_image(t, "interpolate_luma", copy->planes.luma);
    check_image(t, "interpolate_chroma", copy->planes.chroma[0]);
    mb_status status = MB_SUCCESS;
    for (int side = 0; side < SIDES && status == MB_SUCCESS; side++) {
        mb_output_copy c = *copy;
        c.planes = planes_side(copy->planes, side);
        status = OPS(t, side)->record_host_output(SELF(t, side), &c, BUFFER(stats));
    }
    return status;
}

mb_status tee_record_device_output(void *self, const mb_output_copy *copy, mb_buffer *stats)
{
    (void)self;
    (void)copy;
    (void)stats;
    return MB_ERROR_UNSUPPORTED;
}

mb_status tee_backend_create(mb_backend reference, mb_backend device, tee_report *report, mb_backend *out)
{
    tee *t = calloc(1, sizeof(*t));
    if (!t) {
        return MB_ERROR_NO_MEMORY;
    }
    t->backends[REFERENCE] = reference;
    t->backends[DEVICE] = device;
    t->report = report;
    report->count = 0;
    int pending = reference.max_pending < device.max_pending ? reference.max_pending : device.max_pending;
    *out = (mb_backend){&TEE_OPS, t, pending, device.max_blocks};
    return MB_SUCCESS;
}
