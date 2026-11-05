#include "vk_transfers.h"

#include <math.h>
#include <string.h>

#include "motionblade/half.h"
#include "vk_jobs.h"
#include "vk_memory.h"

enum { PLANE_ALIGN = 256 };

static int texel_bytes(mb_texel_format format)
{
    return mb_texel_sample_bytes(format) * mb_texel_channels(format);
}

static plane_layout layout_of_planes(mb_frame_planes planes)
{
    plane_layout layout = {.count = 0};
    size_t offset = 0;
    for (int i = 0; i < 3 && mb_frame_plane(planes, i); i++) {
        const struct mb_image *image = mb_frame_plane(planes, i);
        layout.offsets[i] = offset;
        layout.pixel_bytes[i] = (size_t)texel_bytes(image->format);
        layout.row_bytes[i] = (size_t)image->width * layout.pixel_bytes[i];
        layout.rows[i] = image->height;
        offset += align_up(layout.row_bytes[i] * (size_t)layout.rows[i], PLANE_ALIGN);
        layout.count = i + 1;
    }
    layout.total = offset;
    return layout;
}

bool area_is_cropped(mb_rect area, const mb_video_format *format)
{
    return !mb_rect_equal(area, mb_full_rect(format));
}

bar_rects bars_around(mb_rect inner, const struct mb_image *plane)
{
    int ax = inner.x;
    int ay = inner.y;
    int aw = inner.width;
    int ah = inner.height;
    mb_rect all[4] = {
        {0, 0, plane->width, ay},
        {0, ay + ah, plane->width, plane->height - ay - ah},
        {0, ay, ax, ah},
        {ax + aw, ay, plane->width - ax - aw, ah},
    };
    bar_rects bars = {.count = 0};
    for (int k = 0; k < 4; k++) {
        if (all[k].width > 0 && all[k].height > 0) {
            bars.rects[bars.count++] = all[k];
        }
    }
    return bars;
}

void vk_fill_buffer(void *self, mb_buffer *buffer, uint32_t value)
{
    mbvk_backend *vk = self;
    if (!vk->recording) {
        return;
    }
    resource_use use = {buffer_handle(buffer), true};
    jobs_prepare(vk, &use, 1);
    vkCmdFillBuffer(jobs_tracked_commands(vk), buffer->buffer, 0, VK_WHOLE_SIZE, value);
}

void copy_image_region(mbvk_backend *vk, const image_region *r)
{
    VkImageCopy copy = {
        {r->source->aspect, 0, 0, 1},
        {r->source_x, r->source_y, 0},
        {r->target->aspect, 0, 0, 1},
        {r->target_x, r->target_y, 0},
        {(uint32_t)r->width, (uint32_t)r->height, 1},
    };
    VkCommandBuffer commands = jobs_commands(vk);
    vkCmdCopyImage(commands, r->source->image, r->source->layout, r->target->image, r->target->layout, 1, &copy);
}

void copy_overlap_images(mbvk_backend *vk, const mb_overlap_copy *c)
{
    mb_rect source = {c->source_x, c->source_y, c->source->width, c->source->height};
    mb_rect target = {c->target_x, c->target_y, c->target->width, c->target->height};
    mb_rect overlap = mb_rect_intersection(source, target);
    if (overlap.width <= 0 || overlap.height <= 0) {
        return;
    }
    image_region r = {c->source, overlap.x - c->source_x, overlap.y - c->source_y, c->target, overlap.x - c->target_x,
                      overlap.y - c->target_y, overlap.width, overlap.height};
    copy_image_region(vk, &r);
}

void vk_copy_overlap(void *self, const mb_overlap_copy *copy)
{
    mbvk_backend *vk = self;
    if (!vk->recording) {
        return;
    }
    copy_overlap_images(vk, copy);
    jobs_barrier(vk);
}

typedef struct strided {
    const uint8_t *data;
    ptrdiff_t stride;
} strided;

typedef struct plane_rows {
    int count;
    size_t bytes;
} plane_rows;

static void copy_rows(uint8_t *to, ptrdiff_t to_stride, strided from, plane_rows rows)
{
    for (int y = 0; y < rows.count; y++) {
        memcpy(to + (ptrdiff_t)y * to_stride, from.data + (ptrdiff_t)y * from.stride, rows.bytes);
    }
}

static size_t area_offset(const plane_layout *layout, int plane, mb_rect area, ptrdiff_t stride)
{
    return (size_t)area.y * (size_t)stride + (size_t)area.x * layout->pixel_bytes[plane];
}

static void stage_planes(const mb_upload *upload, const plane_layout *layout, uint8_t *target)
{
    for (int i = 0; i < layout->count; i++) {
        ptrdiff_t stride = upload->source->strides[i];
        mb_rect area = mb_plane_area(upload->format, i, upload->area);
        const uint8_t *from = upload->source->planes[i] + area_offset(layout, i, area, stride);
        copy_rows(target + layout->offsets[i], (ptrdiff_t)layout->row_bytes[i], (strided){from, stride},
                  (plane_rows){layout->rows[i], layout->row_bytes[i]});
    }
}

static void copy_buffer_to_image(mbvk_backend *vk, const struct mb_buffer *from, size_t offset, struct mb_image *to)
{
    VkBufferImageCopy r = {offset, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                           {(uint32_t)to->width, (uint32_t)to->height, 1}};
    vkCmdCopyBufferToImage(jobs_commands(vk), from->buffer, to->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static mb_status claim_staging(mbvk_backend *vk, size_t bytes, staging **out)
{
    int k = vk->next_upload;
    vk->next_upload = (k + 1) % UPLOAD_RING;
    staging *s = &vk->uploads[k];
    job_slot *user = s->slot >= 0 ? &vk->slots[s->slot] : NULL;
    if (user && user->busy && user->serial == s->serial && jobs_wait_slot(vk, s->slot) != MB_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    if (s->buffer.bytes < bytes) {
        buffer_destroy(vk, &s->buffer);
        if (buffer_create_host(vk, &s->buffer, bytes) != MB_SUCCESS) {
            return MB_ERROR_NO_MEMORY;
        }
    }
    s->slot = vk->current;
    s->serial = vk->serial + 1;
    *out = s;
    return MB_SUCCESS;
}

mb_status vk_upload_frame(void *self, const mb_upload *upload)
{
    mbvk_backend *vk = self;
    if (!vk->recording) {
        return MB_ERROR_INVALID;
    }
    plane_layout layout = layout_of_planes(upload->planes);
    staging *s;
    mb_status status = claim_staging(vk, layout.total, &s);
    if (status != MB_SUCCESS) {
        return status;
    }
    stage_planes(upload, &layout, s->buffer.mapped);
    for (int i = 0; i < layout.count; i++) {
        copy_buffer_to_image(vk, &s->buffer, layout.offsets[i], mb_frame_plane(upload->planes, i));
    }
    jobs_barrier(vk);
    return MB_SUCCESS;
}

static void copy_image_to_buffer(mbvk_backend *vk, const struct mb_image *from, const struct mb_buffer *to,
                                 size_t offset)
{
    VkBufferImageCopy r = {offset, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                           {(uint32_t)from->width, (uint32_t)from->height, 1}};
    vkCmdCopyImageToBuffer(jobs_commands(vk), from->image, VK_IMAGE_LAYOUT_GENERAL, to->buffer, 1, &r);
}

void capture_stats(mbvk_backend *vk, mb_buffer *stats)
{
    VkBufferCopy copy = {0, 0, MB_STAT_COUNT * sizeof(uint32_t)};
    vkCmdCopyBuffer(jobs_commands(vk), stats->buffer, jobs_current(vk)->stats_copy.buffer, 1, &copy);
}

void remember_output(job_slot *s, const mb_output_copy *copy, output_target target)
{
    s->target = target;
    s->area = copy->area;
    s->format = *copy->format;
    s->layout = layout_of_planes(copy->planes);
}

mb_status vk_record_host_output(void *self, const mb_output_copy *copy, mb_buffer *stats)
{
    mbvk_backend *vk = self;
    if (!vk->recording) {
        return MB_ERROR_INVALID;
    }
    job_slot *s = jobs_current(vk);
    plane_layout layout = layout_of_planes(copy->planes);
    if (s->readback.bytes < layout.total) {
        buffer_destroy(vk, &s->readback);
        if (buffer_create_host(vk, &s->readback, layout.total) != MB_SUCCESS) {
            return MB_ERROR_NO_MEMORY;
        }
    }
    for (int i = 0; i < layout.count; i++) {
        copy_image_to_buffer(vk, mb_frame_plane(copy->planes, i), &s->readback, layout.offsets[i]);
    }
    capture_stats(vk, stats);
    jobs_host_barrier(vk);
    remember_output(s, copy, TARGET_HOST);
    return MB_SUCCESS;
}

bool vk_output_ready(void *self, mb_ticket ticket)
{
    mbvk_backend *vk = self;
    job_slot *s = jobs_slot_of(vk, ticket);
    return s && (!s->busy || vkGetFenceStatus(vk->device, s->fence) == VK_SUCCESS);
}

static void download(const job_slot *s, const mb_host_image *target, mb_rect area)
{
    const plane_layout *layout = &s->layout;
    for (int i = 0; i < layout->count; i++) {
        ptrdiff_t stride = target->strides[i];
        const uint8_t *from = (const uint8_t *)s->readback.mapped + layout->offsets[i];
        strided source = {from, (ptrdiff_t)layout->row_bytes[i]};
        mb_rect plane = mb_plane_area(&s->format, i, area);
        copy_rows(target->planes[i] + area_offset(layout, i, plane, stride), stride, source,
                  (plane_rows){layout->rows[i], layout->row_bytes[i]});
    }
}

mb_status vk_read_output(void *self, mb_ticket ticket, const mb_host_image *target, mb_rect area)
{
    mbvk_backend *vk = self;
    job_slot *s = jobs_slot_of(vk, ticket);
    if (!s || s->target != TARGET_HOST) {
        return MB_ERROR_INVALID;
    }
    mb_status status = jobs_wait_slot(vk, ticket.slot);
    if (status == MB_SUCCESS) {
        download(s, target, area);
    }
    return status;
}

mb_status vk_finish_output(void *self, mb_ticket ticket, mb_output_stats *stats)
{
    mbvk_backend *vk = self;
    job_slot *s = jobs_slot_of(vk, ticket);
    if (!s) {
        return MB_ERROR_INVALID;
    }
    mb_status status = MB_SUCCESS;
    *stats = (mb_output_stats){0};
    if (s->target != TARGET_SHARED_FRAME) {
        status = jobs_wait_slot(vk, ticket.slot);
        const uint32_t *values = s->stats_copy.mapped;
        *stats = (mb_output_stats){status == MB_SUCCESS, values[MB_STAT_CUT] != 0, values[MB_STAT_ERROR], s->gpu_ms};
    }
    s->holds_output = false;
    return status;
}

static mb_status run_now(mbvk_backend *vk)
{
    jobs_host_barrier(vk);
    mb_status status = jobs_submit(vk);
    return status == MB_SUCCESS ? jobs_wait_idle(vk) : status;
}

mb_status vk_read_buffer(void *self, mb_buffer *buffer, void *data, size_t bytes)
{
    mbvk_backend *vk = self;
    struct mb_buffer copy;
    size_t size = bytes < buffer->bytes ? bytes : buffer->bytes;
    mb_status status = buffer_create_host(vk, &copy, size);
    if (status == MB_SUCCESS) {
        status = jobs_begin(vk);
    }
    if (status == MB_SUCCESS) {
        VkBufferCopy region_copy = {0, 0, size};
        vkCmdCopyBuffer(jobs_commands(vk), buffer->buffer, copy.buffer, 1, &region_copy);
        status = run_now(vk);
    }
    if (status == MB_SUCCESS) {
        memcpy(data, copy.mapped, size);
    }
    buffer_destroy(vk, &copy);
    return status;
}

mb_status vk_write_buffer(void *self, mb_buffer *buffer, const void *data, size_t bytes)
{
    mbvk_backend *vk = self;
    struct mb_buffer staging_copy;
    size_t size = bytes < buffer->bytes ? bytes : buffer->bytes;
    mb_status status = buffer_create_host(vk, &staging_copy, size);
    if (status == MB_SUCCESS) {
        memcpy(staging_copy.mapped, data, size);
        status = jobs_begin(vk);
    }
    if (status == MB_SUCCESS) {
        VkBufferCopy copy = {0, 0, size};
        vkCmdCopyBuffer(jobs_commands(vk), staging_copy.buffer, buffer->buffer, 1, &copy);
        jobs_barrier(vk);
        status = run_now(vk);
    }
    buffer_destroy(vk, &staging_copy);
    return status;
}

typedef struct texel_codec {
    int channels;
    int bytes;
    bool half;
} texel_codec;

static texel_codec codec_of(mb_texel_format format)
{
    int channels = mb_texel_channels(format);
    return (texel_codec){channels, mb_texel_sample_bytes(format), format == MB_TEXELS_R16F};
}

static float decode_texel(const uint8_t *p, texel_codec c)
{
    if (c.bytes == 1) {
        return (float)p[0] / 255.0f;
    }
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return c.half ? mb_half_to_float(v) : (float)v / 65535.0f;
}

static void encode_texel(uint8_t *p, texel_codec c, float value)
{
    if (c.bytes == 1) {
        p[0] = (uint8_t)lrintf(value * 255.0f);
        return;
    }
    uint16_t v = c.half ? mb_half_from_float(value) : (uint16_t)lrintf(value * 65535.0f);
    memcpy(p, &v, sizeof(v));
}

static size_t image_values(const mb_image *image)
{
    return mb_texel_count((mb_image_desc){image->width, image->height, image->format});
}

mb_status vk_read_image(void *self, mb_image *image, float *texels)
{
    mbvk_backend *vk = self;
    texel_codec c = codec_of(image->format);
    struct mb_buffer readback;
    mb_status status = buffer_create_host(vk, &readback, image_values(image) * (size_t)c.bytes);
    if (status == MB_SUCCESS) {
        status = jobs_begin(vk);
    }
    if (status == MB_SUCCESS) {
        copy_image_to_buffer(vk, image, &readback, 0);
        status = run_now(vk);
    }
    for (size_t i = 0; status == MB_SUCCESS && i < image_values(image); i++) {
        texels[i] = decode_texel((const uint8_t *)readback.mapped + i * (size_t)c.bytes, c);
    }
    buffer_destroy(vk, &readback);
    return status;
}

mb_status vk_write_image(void *self, mb_image *image, const float *texels)
{
    mbvk_backend *vk = self;
    texel_codec c = codec_of(image->format);
    struct mb_buffer upload;
    mb_status status = buffer_create_host(vk, &upload, image_values(image) * (size_t)c.bytes);
    for (size_t i = 0; status == MB_SUCCESS && i < image_values(image); i++) {
        encode_texel((uint8_t *)upload.mapped + i * (size_t)c.bytes, c, texels[i]);
    }
    if (status == MB_SUCCESS) {
        status = jobs_begin(vk);
    }
    if (status == MB_SUCCESS) {
        copy_buffer_to_image(vk, &upload, 0, image);
        jobs_barrier(vk);
        status = run_now(vk);
    }
    buffer_destroy(vk, &upload);
    return status;
}
