#include "vk_transfers.h"

#include <math.h>
#include <string.h>

#include "motionblade/half.h"
#include "vk_jobs.h"
#include "vk_memory.h"

enum { PLANE_ALIGN = 256 };

static size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static int texel_bytes(mb_texel_format format)
{
    switch (format) {
    case MB_TEXELS_R8:
        return 1;
    case MB_TEXELS_RG8:
    case MB_TEXELS_R16:
    case MB_TEXELS_R16F:
        return 2;
    case MB_TEXELS_RG16:
        return 4;
    }
    return 1;
}

plane_layout layout_of_planes(mb_frame_planes planes)
{
    plane_layout layout = {.count = 0};
    const struct mb_image *images[3] = {planes.luma, planes.chroma[0], planes.chroma[1]};
    size_t offset = 0;
    for (int i = 0; i < 3 && images[i]; i++) {
        layout.offsets[i] = offset;
        layout.pixel_bytes[i] = (size_t)texel_bytes(images[i]->format);
        layout.row_bytes[i] = (size_t)images[i]->width * layout.pixel_bytes[i];
        layout.rows[i] = images[i]->height;
        offset += align_up(layout.row_bytes[i] * (size_t)layout.rows[i], PLANE_ALIGN);
        layout.count = i + 1;
    }
    layout.total = offset;
    return layout;
}

bool area_is_cropped(mb_rect area, const mb_video_format *format)
{
    return area.x || area.y || area.width != format->width || area.height != format->height;
}

bar_rects bars_around(mb_rect area, plane_extent plane)
{
    int ax = area.x >> plane.shift_x;
    int ay = area.y >> plane.shift_y;
    int aw = (area.width + (1 << plane.shift_x) - 1) >> plane.shift_x;
    int ah = (area.height + (1 << plane.shift_y) - 1) >> plane.shift_y;
    mb_rect all[4] = {
        {0, 0, plane.width, ay},
        {0, ay + ah, plane.width, plane.height - ay - ah},
        {0, ay, ax, ah},
        {ax + aw, ay, plane.width - ax - aw, ah},
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
    vkCmdFillBuffer(jobs_commands(vk), buffer->buffer, 0, VK_WHOLE_SIZE, value);
    jobs_barrier(vk);
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

static void copy_rows(uint8_t *to, ptrdiff_t to_stride, strided from, plane_rows rows)
{
    for (int y = 0; y < rows.count; y++) {
        memcpy(to + (ptrdiff_t)y * to_stride, from.data + (ptrdiff_t)y * from.stride, rows.bytes);
    }
}

static size_t area_offset(const plane_layout *layout, int plane, plane_origin origin, ptrdiff_t stride)
{
    int x = plane ? origin.area.x >> origin.shift_x : origin.area.x;
    int y = plane ? origin.area.y >> origin.shift_y : origin.area.y;
    return (size_t)y * (size_t)stride + (size_t)x * layout->pixel_bytes[plane];
}

static void stage_planes(const mb_upload *upload, const plane_layout *layout, uint8_t *target)
{
    plane_origin origin = {upload->area, upload->format->chroma_shift_x, upload->format->chroma_shift_y};
    for (int i = 0; i < layout->count; i++) {
        ptrdiff_t stride = upload->source->strides[i];
        const uint8_t *from = upload->source->planes[i] + area_offset(layout, i, origin, stride);
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
    struct mb_image *images[3] = {upload->planes.luma, upload->planes.chroma[0], upload->planes.chroma[1]};
    for (int i = 0; i < layout.count; i++) {
        copy_buffer_to_image(vk, &s->buffer, layout.offsets[i], images[i]);
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
    s->shift_x = copy->format->chroma_shift_x;
    s->shift_y = copy->format->chroma_shift_y;
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
    struct mb_image *images[3] = {copy->planes.luma, copy->planes.chroma[0], copy->planes.chroma[1]};
    for (int i = 0; i < layout.count; i++) {
        copy_image_to_buffer(vk, images[i], &s->readback, layout.offsets[i]);
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
    plane_origin origin = {area, s->shift_x, s->shift_y};
    for (int i = 0; i < layout->count; i++) {
        ptrdiff_t stride = target->strides[i];
        const uint8_t *from = (const uint8_t *)s->readback.mapped + layout->offsets[i];
        strided source = {from, (ptrdiff_t)layout->row_bytes[i]};
        copy_rows(target->planes[i] + area_offset(layout, i, origin, stride), stride, source,
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
    int bytes = format == MB_TEXELS_R8 || format == MB_TEXELS_RG8 ? 1 : 2;
    return (texel_codec){channels, bytes, format == MB_TEXELS_R16F};
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
    return (size_t)image->width * (size_t)image->height * (size_t)mb_texel_channels(image->format);
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
