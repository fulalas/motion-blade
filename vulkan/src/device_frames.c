#include "vk_device_frames.h"

#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "push_constants.h"
#include "vk_export.h"
#include "vk_jobs.h"
#include "vk_memory.h"
#include "vk_pipelines.h"
#include "vk_transfers.h"

enum { BAR_TILE = 16, IMPORT_IDLE_LOADS = 2 * MAX_IMPORTS };

typedef struct frame_size {
    int width;
    int height;
} frame_size;

static frame_size plane_size(const mb_video_format *f, int plane)
{
    if (plane == 0) {
        return (frame_size){f->width, f->height};
    }
    return (frame_size){mb_chroma_width(f), mb_chroma_height(f)};
}

static void reset_flags(mbvk_backend *vk)
{
    uint32_t *flags = vk->bg.bar_flags.mapped;
    for (int i = 0; i < vk->bg.flags_count; i++) {
        flags[i] = 1;
    }
}

static mb_status ensure_background(mbvk_backend *vk, const mb_device_load *load)
{
    background *bg = &vk->bg;
    if (bg->bar_flags.buffer) {
        return MB_SUCCESS;
    }
    frame_size luma = plane_size(load->format, 0);
    frame_size chroma = plane_size(load->format, 1);
    mb_image_desc luma_desc = {luma.width, luma.height, load->planes.luma->format};
    mb_image_desc chroma_desc = {chroma.width, chroma.height, load->planes.chroma[0]->format};
    bg->flags_count = luma.width + luma.height;
    bool created = image_create(vk, &bg->luma, &luma_desc) == MB_SUCCESS &&
                   image_create(vk, &bg->chroma, &chroma_desc) == MB_SUCCESS &&
                   buffer_create_host(vk, &bg->bar_flags, (size_t)bg->flags_count * sizeof(uint32_t)) == MB_SUCCESS;
    if (!created) {
        image_destroy(vk, &bg->luma);
        image_destroy(vk, &bg->chroma);
        return MB_ERROR_NO_MEMORY;
    }
    jobs_to_general(vk, &bg->luma);
    jobs_to_general(vk, &bg->chroma);
    reset_flags(vk);
    return MB_SUCCESS;
}

void background_destroy(mbvk_backend *vk)
{
    image_destroy(vk, &vk->bg.luma);
    image_destroy(vk, &vk->bg.chroma);
    buffer_destroy(vk, &vk->bg.bar_flags);
}

#ifndef _WIN32
typedef struct plane_import {
    const mbvk_dmabuf_input *input;
    int plane;
    frame_size size;
    mb_texel_format format;
} plane_import;

static VkImageCreateInfo dmabuf_image_info(struct mb_image *image, const plane_import *p, void *chain)
{
    mb_image_desc desc = {p->size.width, p->size.height, p->format};
    VkImageCreateInfo info = image_info(image, &desc, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    info.pNext = chain;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    return info;
}

static uint32_t import_memory_bits(mbvk_backend *vk, int fd)
{
    if (!vk->features.import_memory_bits) {
        VkMemoryFdPropertiesKHR properties = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vk->get_memory_fd_properties(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &properties) ==
            VK_SUCCESS) {
            vk->features.import_memory_bits = properties.memoryTypeBits;
        }
    }
    return vk->features.import_memory_bits;
}

static mb_status bind_dmabuf(mbvk_backend *vk, struct mb_image *image, int fd)
{
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, image->image, &req);
    uint32_t bits = req.memoryTypeBits & import_memory_bits(vk, fd);
    int owned = bits ? dup(fd) : -1;
    if (owned < 0) {
        return MB_ERROR_UNSUPPORTED;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, image->image,
                                               VK_NULL_HANDLE};
    VkImportMemoryFdInfoKHR import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dedicated,
                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, owned};
    uint32_t type = (uint32_t)__builtin_ctz(bits);
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, req.size, type};
    if (vkAllocateMemory(vk->device, &alloc, NULL, &image->memory) != VK_SUCCESS) {
        close(owned);
        return MB_ERROR_UNSUPPORTED;
    }
    if (vkBindImageMemory(vk->device, image->image, image->memory, 0) != VK_SUCCESS) {
        return MB_ERROR_UNSUPPORTED;
    }
    return image_view(vk, image);
}

static mb_status import_plane(mbvk_backend *vk, const plane_import *p, struct mb_image *image)
{
    const mbvk_dmabuf_input *in = p->input;
    int fd = in->fds[in->objects[p->plane]];
    VkSubresourceLayout subresource = {in->offsets[p->plane], 0, in->pitches[p->plane], 0, 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, in->modifier, 1, &subresource};
    VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modifier,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo info = dmabuf_image_info(image, p, &external);
    mb_status status = MB_ERROR_UNSUPPORTED;
    if (vkCreateImage(vk->device, &info, NULL, &image->image) == VK_SUCCESS) {
        status = bind_dmabuf(vk, image, fd);
    }
    if (status != MB_SUCCESS) {
        image_destroy(vk, image);
    }
    return status;
}

static mb_status import_dmabuf(mbvk_backend *vk, const mb_device_load *load, struct mb_image images[2])
{
    const mbvk_dmabuf_input *in = load->frame->payload;
    plane_import luma = {in, 0, plane_size(load->format, 0), load->planes.luma->format};
    plane_import chroma = {in, 1, plane_size(load->format, 1), load->planes.chroma[0]->format};
    mb_status status = import_plane(vk, &luma, &images[0]);
    if (status == MB_SUCCESS) {
        status = import_plane(vk, &chroma, &images[1]);
    }
    if (status != MB_SUCCESS) {
        image_destroy(vk, &images[0]);
    }
    return status;
}

static bool key_of(const mb_device_load *load, import_key *key)
{
    const mbvk_dmabuf_input *in = load->frame->payload;
    *key = (import_key){.modifier = in->modifier, .width = load->format->width, .height = load->format->height};
    for (int plane = 0; plane < 2; plane++) {
        struct stat st;
        if (fstat(in->fds[in->objects[plane]], &st) != 0) {
            return false;
        }
        key->devices[plane] = (uint64_t)st.st_dev;
        key->inodes[plane] = (uint64_t)st.st_ino;
        key->offsets[plane] = in->offsets[plane];
        key->pitches[plane] = in->pitches[plane];
    }
    return true;
}
#else
static mb_status import_dmabuf(mbvk_backend *vk, const mb_device_load *load, struct mb_image images[2])
{
    (void)vk;
    (void)load;
    (void)images;
    return MB_ERROR_UNSUPPORTED;
}

static bool key_of(const mb_device_load *load, import_key *key)
{
    (void)load;
    (void)key;
    return false;
}
#endif

static void transfer_ownership(mbvk_backend *vk, struct mb_image images[2], bool acquire)
{
    uint32_t external = vk->features.foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
    VkAccessFlags use = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkImageMemoryBarrier b[2];
    for (int k = 0; k < 2; k++) {
        b[k] = image_barrier(images[k].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        b[k].srcAccessMask = acquire ? 0 : use;
        b[k].dstAccessMask = acquire ? use : 0;
        b[k].srcQueueFamilyIndex = acquire ? external : vk->queue_family;
        b[k].dstQueueFamilyIndex = acquire ? vk->queue_family : external;
    }
    VkPipelineStageFlags work = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(jobs_commands(vk), acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : work,
                         acquire ? work : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 2, b);
}

typedef struct source_planes {
    struct mb_image *luma;
    struct mb_image *chroma;
} source_planes;

static void copy_active_area(mbvk_backend *vk, const mb_device_load *load, source_planes source)
{
    mb_rect area = mb_plane_area(load->format, 1, load->area);
    mb_overlap_copy luma = {source.luma, load->planes.luma, -load->area.x, -load->area.y, 0, 0};
    mb_overlap_copy chroma = {source.chroma, load->planes.chroma[0], -area.x, -area.y, 0, 0};
    copy_overlap_images(vk, &luma);
    copy_overlap_images(vk, &chroma);
}

static void refresh_background(mbvk_backend *vk, source_planes source)
{
    mb_overlap_copy luma = {source.luma, &vk->bg.luma, 0, 0, 0, 0};
    mb_overlap_copy chroma = {source.chroma, &vk->bg.chroma, 0, 0, 0, 0};
    copy_overlap_images(vk, &luma);
    copy_overlap_images(vk, &chroma);
}

static void detect_bars(mbvk_backend *vk, const mb_device_load *load, source_planes source)
{
    if (!source.luma->view) {
        jobs_barrier(vk);
        source = (source_planes){&vk->bg.luma, &vk->bg.chroma};
    }
    struct bar_flags_push push = {load->format->width, load->format->height, load->format->chroma_shift_x,
                                  load->format->chroma_shift_y, MB_BLACK_LUMA_MAX / 255.0f,
                                  MB_BLACK_CHROMA_TOLERANCE / 255.0f};
    binding b[] = {{source.luma, NULL}, {source.chroma, NULL}, {NULL, &vk->bg.bar_flags}};
    uint32_t gx = (uint32_t)(source.chroma->width + BAR_TILE - 1) / BAR_TILE;
    uint32_t gy = (uint32_t)(source.chroma->height + BAR_TILE - 1) / BAR_TILE;
    dispatch_desc desc = {PASS_BAR_FLAGS, BINDINGS(b), PUSH(push), NULL, gx, gy};
    pipeline_dispatch(vk, &desc);
    vk->bg.bars_requested = true;
    vk->bg.bar_slot = vk->current;
    vk->bg.bar_serial = vk->serial + 1;
}

static void record_load(mbvk_backend *vk, const mb_device_load *load, source_planes source)
{
    copy_active_area(vk, load, source);
    if (load->refresh_border || (load->detect_bars && !source.luma->view)) {
        refresh_background(vk, source);
    }
    if (load->detect_bars) {
        detect_bars(vk, load, source);
    }
}

static bool import_empty(const cached_import *e)
{
    return !e->images[0].image;
}

static bool import_in_flight(const mbvk_backend *vk, const cached_import *e)
{
    const job_slot *s = &vk->slots[e->slot];
    bool recording = vk->recording && vk->current == e->slot && e->serial == vk->serial + 1;
    return recording || (s->busy && s->serial == e->serial);
}

static void forget_import(mbvk_backend *vk, cached_import *e)
{
    image_destroy(vk, &e->images[0]);
    image_destroy(vk, &e->images[1]);
    *e = (cached_import){.slot = 0};
}

static cached_import *find_import(import_cache *cache, const import_key *key)
{
    for (int i = 0; i < cache->count; i++) {
        cached_import *e = &cache->entries[i];
        if (!import_empty(e) && memcmp(&e->key, key, sizeof(*key)) == 0) {
            return e;
        }
    }
    return NULL;
}

static cached_import *oldest_idle_import(mbvk_backend *vk)
{
    cached_import *oldest = NULL;
    for (int i = 0; i < vk->imports.count; i++) {
        cached_import *e = &vk->imports.entries[i];
        if (import_empty(e)) {
            return e;
        }
        if (!import_in_flight(vk, e) && (!oldest || e->used_at < oldest->used_at)) {
            oldest = e;
        }
    }
    return oldest;
}

static cached_import *claim_import(mbvk_backend *vk)
{
    import_cache *cache = &vk->imports;
    if (cache->count < MAX_IMPORTS) {
        cached_import *e = &cache->entries[cache->count++];
        *e = (cached_import){.slot = 0};
        return e;
    }
    cached_import *e = oldest_idle_import(vk);
    if (e) {
        forget_import(vk, e);
    }
    return e;
}

static void reclaim_idle_imports(mbvk_backend *vk)
{
    for (int i = 0; i < vk->imports.count; i++) {
        cached_import *e = &vk->imports.entries[i];
        bool idle = vk->imports.clock - e->used_at > IMPORT_IDLE_LOADS;
        if (!import_empty(e) && idle && !import_in_flight(vk, e)) {
            forget_import(vk, e);
        }
    }
}

static mb_status import_cached(mbvk_backend *vk, const mb_device_load *load, struct mb_image **planes)
{
    import_key key;
    if (!key_of(load, &key)) {
        return MB_ERROR_BUSY;
    }
    cached_import *e = find_import(&vk->imports, &key);
    if (!e) {
        e = claim_import(vk);
        if (!e) {
            return MB_ERROR_BUSY;
        }
        mb_status status = import_dmabuf(vk, load, e->images);
        if (status != MB_SUCCESS) {
            forget_import(vk, e);
            return status;
        }
        e->key = key;
    }
    e->slot = vk->current;
    e->serial = vk->serial + 1;
    e->used_at = ++vk->imports.clock;
    *planes = e->images;
    reclaim_idle_imports(vk);
    return MB_SUCCESS;
}

void imports_destroy(mbvk_backend *vk)
{
    for (int i = 0; i < vk->imports.count; i++) {
        forget_import(vk, &vk->imports.entries[i]);
    }
    vk->imports.count = 0;
}

static mb_status import_planes(mbvk_backend *vk, const mb_device_load *load, struct mb_image **planes)
{
    mb_status status = import_cached(vk, load, planes);
    if (status != MB_ERROR_BUSY) {
        return status;
    }
    job_slot *slot = jobs_current(vk);
    *planes = slot->imports;
    return import_dmabuf(vk, load, slot->imports);
}

static mb_status load_dmabuf(mbvk_backend *vk, const mb_device_load *load)
{
    const mbvk_dmabuf_input *in = load->frame->payload;
    if (!vk->features.modifiers || !load->planes.chroma[0] || load->planes.chroma[1]) {
        return MB_ERROR_UNSUPPORTED;
    }
    jobs_poll(vk);
    struct mb_image *planes;
    mb_status status = import_planes(vk, load, &planes);
    if (status != MB_SUCCESS) {
        return status;
    }
    if (ensure_background(vk, load) != MB_SUCCESS) {
        return MB_ERROR_NO_MEMORY;
    }
    transfer_ownership(vk, planes, true);
    record_load(vk, load, (source_planes){&planes[0], &planes[1]});
    transfer_ownership(vk, planes, false);
    jobs_host_barrier(vk);
    if (in->release) {
        jobs_add_release(vk, in->release, in->context);
    }
    return MB_SUCCESS;
}

static bool shared_frame_usable(const mbvk_backend *vk, const mbvk_vulkan_frame *f)
{
    if (!vk->uses_shared || f->image_count < 1 || f->image_count > 2) {
        return false;
    }
    if (vk->waits.count + f->image_count > MAX_SEMAPHORES || vk->signals.count + f->image_count > MAX_SEMAPHORES) {
        return false;
    }
    for (int i = 0; i < f->image_count; i++) {
        if (f->queue_families[i] != VK_QUEUE_FAMILY_IGNORED && f->queue_families[i] != vk->queue_family) {
            return false;
        }
    }
    return true;
}

typedef struct plane_context {
    const mb_video_format *format;
    VkImageLayout layout;
} plane_context;

static struct mb_image frame_plane(const mbvk_vulkan_frame *f, int plane, const plane_context *like)
{
    struct mb_image image = {.image = f->images[f->image_count == 2 ? plane : 0], .layout = like->layout};
    frame_size size = plane_size(like->format, plane);
    image.width = size.width;
    image.height = size.height;
    image.aspect = f->image_count == 2 ? VK_IMAGE_ASPECT_COLOR_BIT
                   : plane           ? VK_IMAGE_ASPECT_PLANE_1_BIT
                                     : VK_IMAGE_ASPECT_PLANE_0_BIT;
    return image;
}

static void begin_shared_frame(mbvk_backend *vk, mbvk_vulkan_frame *f, VkImageLayout layout, VkAccessFlags access)
{
    VkImageMemoryBarrier b[2];
    for (int i = 0; i < f->image_count; i++) {
        b[i] = image_barrier(f->images[i], f->layouts[i], layout);
        b[i].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b[i].dstAccessMask = access;
        vk->waits.semaphores[vk->waits.count] = f->semaphores[i];
        vk->waits.values[vk->waits.count++] = f->semaphore_values[i];
    }
    vkCmdPipelineBarrier(jobs_commands(vk), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL, (uint32_t)f->image_count, b);
}

static void end_shared_frame(mbvk_backend *vk, mbvk_vulkan_frame *f, VkImageLayout layout)
{
    for (int i = 0; i < f->image_count; i++) {
        f->layouts[i] = layout;
        vk->signals.semaphores[vk->signals.count] = f->semaphores[i];
        vk->signals.values[vk->signals.count++] = ++f->semaphore_values[i];
    }
    VkMemoryBarrier b = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    VkPipelineStageFlags after = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vkCmdPipelineBarrier(jobs_commands(vk), VK_PIPELINE_STAGE_TRANSFER_BIT, after, 0, 1, &b, 0, NULL, 0, NULL);
}

static mb_status load_shared_frame(mbvk_backend *vk, const mb_device_load *load)
{
    mbvk_vulkan_frame *f = load->frame->payload;
    if (!shared_frame_usable(vk, f) || !load->planes.chroma[0] || load->planes.chroma[1]) {
        return MB_ERROR_UNSUPPORTED;
    }
    jobs_poll(vk);
    if (ensure_background(vk, load) != MB_SUCCESS) {
        return MB_ERROR_NO_MEMORY;
    }
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    begin_shared_frame(vk, f, layout, VK_ACCESS_TRANSFER_READ_BIT);
    plane_context like = {load->format, layout};
    struct mb_image luma = frame_plane(f, 0, &like);
    struct mb_image chroma = frame_plane(f, 1, &like);
    record_load(vk, load, (source_planes){&luma, &chroma});
    end_shared_frame(vk, f, layout);
    return MB_SUCCESS;
}

mb_status vk_load_device_frame(void *self, const mb_device_load *load)
{
    mbvk_backend *vk = self;
    if (!vk->recording) {
        return MB_ERROR_INVALID;
    }
    switch (load->frame->kind) {
    case MBVK_FRAME_DMABUF:
        return load_dmabuf(vk, load);
    case MBVK_FRAME_VULKAN:
        return load_shared_frame(vk, load);
    default:
        return MB_ERROR_UNSUPPORTED;
    }
}

void vk_fill_from_border(void *self, mb_frame_planes planes, mb_rect area, const mb_video_format *format)
{
    mbvk_backend *vk = self;
    if (!vk->recording || !vk->bg.luma.image) {
        return;
    }
    mb_rect chroma_area = mb_plane_area(format, 1, area);
    mb_overlap_copy luma = {&vk->bg.luma, planes.luma, 0, 0, area.x, area.y};
    mb_overlap_copy chroma = {&vk->bg.chroma, planes.chroma[0], 0, 0, chroma_area.x, chroma_area.y};
    copy_overlap_images(vk, &luma);
    copy_overlap_images(vk, &chroma);
    jobs_barrier(vk);
}

bool vk_poll_bar_flags(void *self, const uint32_t **flags)
{
    mbvk_backend *vk = self;
    background *bg = &vk->bg;
    *flags = NULL;
    if (!bg->bars_requested) {
        return false;
    }
    job_slot *s = &vk->slots[bg->bar_slot];
    if ((vk->recording && vk->current == bg->bar_slot) || (s->busy && s->serial == bg->bar_serial)) {
        return false;
    }
    bg->bars_requested = false;
    *flags = bg->bar_flags.mapped;
    return true;
}

void vk_clear_bar_flags(void *self)
{
    mbvk_backend *vk = self;
    if (vk->bg.bar_flags.mapped) {
        reset_flags(vk);
    }
}

static void copy_bars_into_frame(mbvk_backend *vk, const mb_output_copy *copy, struct mb_image *target, int plane)
{
    struct mb_image *source = plane ? &vk->bg.chroma : &vk->bg.luma;
    bar_rects bars = bars_around(mb_plane_area(copy->format, plane, copy->area), target);
    for (int k = 0; k < bars.count; k++) {
        mb_rect r = bars.rects[k];
        image_region region = {source, r.x, r.y, target, r.x, r.y, r.width, r.height};
        copy_image_region(vk, &region);
    }
}

static mb_status record_shared_output(mbvk_backend *vk, const mb_output_copy *copy)
{
    mbvk_vulkan_frame *f = copy->target->payload;
    if (!shared_frame_usable(vk, f)) {
        return MB_ERROR_UNSUPPORTED;
    }
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    begin_shared_frame(vk, f, layout, VK_ACCESS_TRANSFER_WRITE_BIT);
    plane_context like = {copy->format, layout};
    struct mb_image *outputs[2] = {copy->planes.luma, copy->planes.chroma[0]};
    for (int i = 0; i < 2; i++) {
        struct mb_image target = frame_plane(f, i, &like);
        if (area_is_cropped(copy->area, copy->format) && vk->bg.luma.image) {
            copy_bars_into_frame(vk, copy, &target, i);
        }
        mb_rect area = mb_plane_area(copy->format, i, copy->area);
        image_region region = {outputs[i], 0, 0, &target, area.x, area.y, outputs[i]->width, outputs[i]->height};
        copy_image_region(vk, &region);
    }
    end_shared_frame(vk, f, layout);
    return MB_SUCCESS;
}

mb_status vk_record_device_output(void *self, const mb_output_copy *copy, mb_buffer *stats)
{
    mbvk_backend *vk = self;
    if (!vk->recording) {
        return MB_ERROR_INVALID;
    }
    mb_status status = MB_ERROR_UNSUPPORTED;
    output_target target = TARGET_NONE;
    if (copy->target->kind == MBVK_FRAME_VULKAN) {
        status = record_shared_output(vk, copy);
        target = TARGET_SHARED_FRAME;
    } else if (copy->target->kind == MBVK_FRAME_EXPORT) {
        status = export_record_output(vk, copy);
        target = TARGET_EXPORT;
    }
    if (status != MB_SUCCESS) {
        return status;
    }
    capture_stats(vk, stats);
    jobs_host_barrier(vk);
    remember_output(jobs_current(vk), copy, target);
    return MB_SUCCESS;
}
