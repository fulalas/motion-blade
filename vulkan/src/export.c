#include "vk_export.h"

#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "vk_device_frames.h"
#include "vk_jobs.h"
#include "vk_memory.h"
#include "vk_transfers.h"

enum { MIN_BUFFERS = 4, GROW_LIMIT = 8, IDLE_RECLAIM = 64, PITCH_ALIGN = 256, PLANE_ALIGN = 4096 };

static void release_index(export_pool *pool, int index)
{
    atomic_store(&pool->released_at[index], atomic_fetch_add(&pool->clock, 1) + 1);
    atomic_store(&pool->busy[index], 0);
}

static void drop_reference(export_pool *pool)
{
    if (atomic_fetch_sub(&pool->references, 1) == 1) {
        free(pool);
    }
}

#ifndef _WIN32
static int exportable_memory_type(const mbvk_backend *vk, uint32_t bits)
{
    VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = memory_type(vk, bits, visible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    return type >= 0 ? type : memory_type(vk, bits, visible, 0);
}

static mb_status allocate_exportable(mbvk_backend *vk, struct mb_buffer *b, size_t bytes)
{
    VkExternalMemoryBufferCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(vk->device, &info, NULL, &b->buffer) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, b->buffer, &req);
    int type = exportable_memory_type(vk, req.memoryTypeBits);
    if (type < 0) {
        return MB_ERROR_UNSUPPORTED;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE,
                                               b->buffer};
    VkExportMemoryAllocateInfo export_info = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated,
                                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info, req.size, (uint32_t)type};
    if (vkAllocateMemory(vk->device, &alloc, NULL, &b->memory) != VK_SUCCESS ||
        vkBindBufferMemory(vk->device, b->buffer, b->memory, 0) != VK_SUCCESS ||
        vkMapMemory(vk->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS) {
        return MB_ERROR_NO_MEMORY;
    }
    b->bytes = bytes;
    return MB_SUCCESS;
}

static mb_status create_export_buffer(mbvk_backend *vk, struct mbvk_export_buffer *x)
{
    memset(x, 0, sizeof(*x));
    x->fd = -1;
    x->border_version = -1;
    mb_status status = allocate_exportable(vk, &x->memory, vk->out.size);
    VkMemoryGetFdInfoKHR get = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, x->memory.memory,
                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    if (status == MB_SUCCESS && vk->get_memory_fd(vk->device, &get, &x->fd) != VK_SUCCESS) {
        status = MB_ERROR_DEVICE;
    }
    return status;
}

static void close_fd(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

static int duplicate_fd(int fd)
{
    return dup(fd);
}
#else
static mb_status create_export_buffer(mbvk_backend *vk, struct mbvk_export_buffer *x)
{
    (void)vk;
    memset(x, 0, sizeof(*x));
    x->fd = -1;
    return MB_ERROR_UNSUPPORTED;
}

static void close_fd(int fd)
{
    (void)fd;
}

static int duplicate_fd(int fd)
{
    (void)fd;
    return -1;
}
#endif

static void destroy_export_buffer(mbvk_backend *vk, struct mbvk_export_buffer *x)
{
    buffer_destroy(vk, &x->memory);
    close_fd(x->fd);
    x->fd = -1;
}

static int add_buffer(mbvk_backend *vk, int index)
{
    struct mbvk_export_buffer *x = &vk->out.buffers[index];
    if (create_export_buffer(vk, x) != MB_SUCCESS) {
        destroy_export_buffer(vk, x);
        return -1;
    }
    x->index = index;
    export_pool *pool = vk->out.pool;
    atomic_store(&pool->busy[index], 1);
    atomic_store(&pool->released_at[index], atomic_load(&pool->clock) - (IDLE_RECLAIM - 1));
    if (index == vk->out.count) {
        vk->out.count++;
    }
    return index;
}

static void plan_layout(exports *out, const mb_video_format *format)
{
    size_t offset = 0;
    for (int i = 0; i < mb_plane_count(format); i++) {
        out->pitches[i] = align_up(mb_plane_row_bytes(format, i), PITCH_ALIGN);
        out->offsets[i] = offset;
        offset += align_up(out->pitches[i] * (size_t)mb_plane_rows(format, i), PLANE_ALIGN);
    }
    out->size = offset;
    out->format = *format;
}

mb_status mbvk_export_enable(mbvk_backend *vk, const mb_video_format *format)
{
    bool normalized = mb_bytes_per_sample(format) == 2 && format->bit_depth < 16;
    if (!vk->healthy || !vk->features.dmabuf || !vk->get_memory_fd || normalized) {
        return MB_ERROR_UNSUPPORTED;
    }
    if (vk->out.pool) {
        return MB_SUCCESS;
    }
    plan_layout(&vk->out, format);
    vk->out.pool = calloc(1, sizeof(*vk->out.pool));
    if (!vk->out.pool) {
        return MB_ERROR_NO_MEMORY;
    }
    atomic_init(&vk->out.pool->references, 1);
    for (int i = 0; i < MIN_BUFFERS && add_buffer(vk, vk->out.count) >= 0; i++) {
    }
    for (int i = 0; i < vk->out.count; i++) {
        atomic_store(&vk->out.pool->busy[i], 0);
    }
    return vk->out.count ? MB_SUCCESS : MB_ERROR_UNSUPPORTED;
}

typedef struct pool_scan {
    int best;
    unsigned age;
    int hole;
} pool_scan;

static pool_scan scan_pool(mbvk_backend *vk)
{
    export_pool *pool = vk->out.pool;
    unsigned now = atomic_load(&pool->clock);
    pool_scan scan = {-1, 0, -1};
    for (int i = 0; i < vk->out.count; i++) {
        if (!vk->out.buffers[i].memory.buffer) {
            scan.hole = scan.hole < 0 ? i : scan.hole;
            continue;
        }
        if (atomic_load(&pool->busy[i])) {
            continue;
        }
        unsigned age = now - atomic_load(&pool->released_at[i]);
        if (i >= MIN_BUFFERS && age > IDLE_RECLAIM) {
            destroy_export_buffer(vk, &vk->out.buffers[i]);
            scan.hole = scan.hole < 0 ? i : scan.hole;
            continue;
        }
        if (scan.best < 0 || age > scan.age) {
            scan.best = i;
            scan.age = age;
        }
    }
    return scan;
}

static int acquire_index(mbvk_backend *vk)
{
    pool_scan scan = scan_pool(vk);
    int grow = scan.hole >= 0 ? scan.hole : vk->out.count < MAX_EXPORTS ? vk->out.count : -1;
    bool want_more = scan.best < 0 || (scan.age < 2 && vk->out.count < GROW_LIMIT);
    if (want_more && grow >= 0 && add_buffer(vk, grow) >= 0) {
        return grow;
    }
    int idle = 0;
    if (scan.best < 0 || !atomic_compare_exchange_strong(&vk->out.pool->busy[scan.best], &idle, 1)) {
        return -1;
    }
    return scan.best;
}

mb_status mbvk_export_acquire(mbvk_backend *vk, mbvk_export_buffer **buffer)
{
    if (!vk->out.pool) {
        return MB_ERROR_UNSUPPORTED;
    }
    int index = acquire_index(vk);
    if (index < 0) {
        return MB_ERROR_BUSY;
    }
    *buffer = &vk->out.buffers[index];
    return MB_SUCCESS;
}

static mb_host_image mapped_image(const exports *out, const struct mbvk_export_buffer *x)
{
    mb_host_image image = {{NULL}, {0}};
    for (int i = 0; i < mb_plane_count(&out->format); i++) {
        image.planes[i] = (uint8_t *)x->memory.mapped + out->offsets[i];
        image.strides[i] = (ptrdiff_t)out->pitches[i];
    }
    return image;
}

mb_status mbvk_export_host_image(mbvk_backend *vk, const mb_host_image *source, mbvk_export_buffer **buffer)
{
    mb_status status = mbvk_export_acquire(vk, buffer);
    if (status != MB_SUCCESS) {
        return status;
    }
    mb_host_image target = mapped_image(&vk->out, *buffer);
    mb_host_image_copy(&target, source, &vk->out.format);
    (*buffer)->border_version = vk->out.border_version;
    return MB_SUCCESS;
}

void mbvk_export_read(mbvk_backend *vk, const mbvk_export_buffer *buffer, const mb_host_image *target)
{
    mb_host_image source = mapped_image(&vk->out, buffer);
    mb_host_image_copy(target, &source, &vk->out.format);
}

mb_device_frame mbvk_export_target(mbvk_export_buffer *buffer)
{
    return (mb_device_frame){MBVK_FRAME_EXPORT, buffer};
}

mb_status mbvk_export_describe(mbvk_backend *vk, mbvk_export_buffer *buffer, mbvk_dmabuf_output *output)
{
    memset(output, 0, sizeof(*output));
    output->fd = duplicate_fd(buffer->fd);
    if (output->fd < 0) {
        release_index(vk->out.pool, buffer->index);
        return MB_ERROR_DEVICE;
    }
    output->plane_count = mb_plane_count(&vk->out.format);
    for (int k = 0; k < output->plane_count; k++) {
        output->offsets[k] = (uint32_t)vk->out.offsets[k];
        output->pitches[k] = (uint32_t)vk->out.pitches[k];
    }
    output->size = vk->out.size;
    output->pool = vk->out.pool;
    output->index = buffer->index;
    atomic_fetch_add(&vk->out.pool->references, 1);
    return MB_SUCCESS;
}

void mbvk_export_cancel(mbvk_backend *vk, mbvk_export_buffer *buffer)
{
    release_index(vk->out.pool, buffer->index);
}

void mbvk_export_release(void *pool, int index)
{
    release_index(pool, index);
    drop_reference(pool);
}

void exports_destroy(mbvk_backend *vk)
{
    for (int i = 0; i < vk->out.count; i++) {
        destroy_export_buffer(vk, &vk->out.buffers[i]);
    }
    if (vk->out.pool) {
        drop_reference(vk->out.pool);
    }
    vk->out.pool = NULL;
}

typedef struct export_rect {
    int plane;
    int source_x;
    int source_y;
    int target_x;
    int target_y;
    int width;
    int height;
} export_rect;

static void copy_rect_to_buffer(mbvk_backend *vk, struct mb_image *image, const struct mbvk_export_buffer *x,
                                const export_rect *r)
{
    const exports *out = &vk->out;
    int components = r->plane && out->format.interleaved_chroma ? 2 : 1;
    uint32_t pixel = (uint32_t)(components * mb_bytes_per_sample(&out->format));
    VkBufferImageCopy copy = {out->offsets[r->plane] + (size_t)r->target_y * out->pitches[r->plane] +
                                  (size_t)r->target_x * pixel,
                              (uint32_t)(out->pitches[r->plane] / pixel), 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              {r->source_x, r->source_y, 0}, {(uint32_t)r->width, (uint32_t)r->height, 1}};
    vkCmdCopyImageToBuffer(jobs_commands(vk), image->image, VK_IMAGE_LAYOUT_GENERAL, x->memory.buffer, 1, &copy);
}

static void fill_bars_from_device(mbvk_backend *vk, const mb_output_copy *copy, struct mbvk_export_buffer *x)
{
    struct mb_image *images[2] = {&vk->bg.luma, &vk->bg.chroma};
    for (int i = 0; i < 2; i++) {
        bar_rects bars = bars_around(mb_plane_area(copy->format, i, copy->area), images[i]);
        for (int k = 0; k < bars.count; k++) {
            mb_rect b = bars.rects[k];
            export_rect r = {i, b.x, b.y, b.x, b.y, b.width, b.height};
            copy_rect_to_buffer(vk, images[i], x, &r);
        }
    }
}

static void fill_bars(mbvk_backend *vk, const mb_output_copy *copy, struct mbvk_export_buffer *x)
{
    if (copy->border.version != MB_BORDER_FROM_FRAME && x->border_version == copy->border.version) {
        return;
    }
    if (!copy->border.host_template) {
        fill_bars_from_device(vk, copy, x);
    } else {
        mb_host_image target = mapped_image(&vk->out, x);
        mb_host_image_copy_outside(&target, copy->border.host_template, copy->format, copy->area);
    }
    x->border_version = copy->border.version;
}

mb_status export_record_output(mbvk_backend *vk, const mb_output_copy *copy)
{
    struct mbvk_export_buffer *x = copy->target->payload;
    if (copy->border.version != MB_BORDER_FROM_FRAME) {
        vk->out.border_version = copy->border.version;
    }
    if (area_is_cropped(copy->area, copy->format)) {
        fill_bars(vk, copy, x);
    }
    for (int i = 0; i < mb_plane_count(&vk->out.format); i++) {
        struct mb_image *image = mb_frame_plane(copy->planes, i);
        mb_rect area = mb_plane_area(copy->format, i, copy->area);
        export_rect r = {i, 0, 0, area.x, area.y, image->width, image->height};
        copy_rect_to_buffer(vk, image, x, &r);
    }
    return MB_SUCCESS;
}
