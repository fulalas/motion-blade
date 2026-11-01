#ifndef MB_TEST_TEE_INTERNAL_H
#define MB_TEST_TEE_INTERNAL_H

#include "tee_backend.h"

enum { REFERENCE, DEVICE, SIDES, TICKETS = 64 };

struct mb_image {
    void *side[SIDES];
    int width;
    int height;
    mb_texel_format format;
};

struct mb_buffer {
    void *side[SIDES];
    size_t bytes;
};

typedef struct tee {
    mb_backend backends[SIDES];
    tee_report *report;
    mb_ticket device_tickets[TICKETS];
} tee;

typedef enum compare_kind {
    COMPARE_FIELD,
    COMPARE_WORDS,
    COMPARE_FLOATS,
} compare_kind;

void *image_side(const mb_image *image, int side);
void *buffer_side(const mb_buffer *buffer, int side);
mb_frame_planes planes_side(mb_frame_planes planes, int side);
void check_buffer(tee *t, const char *name, mb_buffer *buffer, compare_kind kind);
void check_image(tee *t, const char *name, mb_image *image);

#define IMAGE(h) ((mb_image *)image_side((h), side))
#define BUFFER(h) ((mb_buffer *)buffer_side((h), side))
#define OPS(t, side) ((t)->backends[side].ops)
#define SELF(t, side) ((t)->backends[side].self)

void tee_destroy(void *self);
mb_status tee_create_image(void *self, const mb_image_desc *desc, mb_image **out);
void tee_destroy_image(void *self, mb_image *image);
mb_status tee_create_buffer(void *self, size_t bytes, mb_buffer **out);
void tee_destroy_buffer(void *self, mb_buffer *buffer);
mb_status tee_read_buffer(void *self, mb_buffer *buffer, void *data, size_t bytes);
mb_status tee_write_buffer(void *self, mb_buffer *buffer, const void *data, size_t bytes);
mb_status tee_read_image(void *self, mb_image *image, float *texels);
mb_status tee_write_image(void *self, mb_image *image, const float *texels);
mb_status tee_begin_work(void *self);
mb_status tee_submit_work(void *self);
mb_status tee_submit_output(void *self, mb_ticket *ticket);
mb_status tee_wait_idle(void *self);
bool tee_output_ready(void *self, mb_ticket ticket);
mb_status tee_read_output(void *self, mb_ticket ticket, const mb_host_image *target, mb_rect area);
mb_status tee_finish_output(void *self, mb_ticket ticket, mb_output_stats *stats);
void tee_fill_buffer(void *self, mb_buffer *buffer, uint32_t value);
void tee_copy_overlap(void *self, const mb_overlap_copy *copy);
mb_status tee_upload_frame(void *self, const mb_upload *upload);
mb_status tee_load_device_frame(void *self, const mb_device_load *load);
void tee_fill_from_border(void *self, mb_frame_planes planes, mb_rect area, const mb_video_format *format);
bool tee_poll_bar_flags(void *self, const uint32_t **flags);
void tee_clear_bar_flags(void *self);
mb_status tee_record_host_output(void *self, const mb_output_copy *copy, mb_buffer *stats);
mb_status tee_record_device_output(void *self, const mb_output_copy *copy, mb_buffer *stats);

extern const mb_backend_ops TEE_OPS;

#endif
