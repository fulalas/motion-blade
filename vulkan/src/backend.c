#include <stdlib.h>
#include <string.h>

#include "vk_device.h"
#include "vk_device_frames.h"
#include "vk_export.h"
#include "vk_jobs.h"
#include "vk_memory.h"
#include "vk_passes.h"
#include "vk_pipelines.h"
#include "vk_transfers.h"

enum { BLOCKS_PER_GROUP = 4, DUMMY_BYTES = 64 };

static void vk_destroy(void *self)
{
    mbvk_backend *vk = self;
    if (vk->prof.enabled) {
        jobs_print_profile(vk);
    }
    if (vk->device) {
        jobs_submit(vk);
        if (vk->uses_shared) {
            jobs_wait_idle(vk);
        } else {
            vkDeviceWaitIdle(vk->device);
        }
        jobs_destroy(vk);
        background_destroy(vk);
        imports_destroy(vk);
        exports_destroy(vk);
        for (int k = 0; k < UPLOAD_RING; k++) {
            buffer_destroy(vk, &vk->uploads[k].buffer);
        }
        buffer_destroy(vk, &vk->dummy);
        image_destroy(vk, &vk->dummy_image);
        pipelines_destroy(vk);
        if (vk->sampler) {
            vkDestroySampler(vk->device, vk->sampler, NULL);
        }
    }
    device_close(vk);
    free(vk);
}

static void vk_prepare(void *self, const mb_video_format *format)
{
    pipelines_prepare(self, format);
}

static mb_status vk_create_image(void *self, const mb_image_desc *desc, mb_image **image)
{
    mbvk_backend *vk = self;
    mb_image *created = calloc(1, sizeof(*created));
    if (!created) {
        return MB_ERROR_NO_MEMORY;
    }
    mb_status status = jobs_begin(vk);
    if (status == MB_SUCCESS) {
        status = image_create(vk, created, desc);
    }
    if (status != MB_SUCCESS) {
        free(created);
        return status;
    }
    jobs_to_general(vk, created);
    *image = created;
    return MB_SUCCESS;
}

static void vk_destroy_image(void *self, mb_image *image)
{
    if (image) {
        image_destroy(self, image);
        free(image);
    }
}

static mb_status vk_create_buffer(void *self, size_t bytes, mb_buffer **buffer)
{
    mb_buffer *created = calloc(1, sizeof(*created));
    if (!created) {
        return MB_ERROR_NO_MEMORY;
    }
    mb_status status = buffer_create_device(self, created, bytes);
    if (status != MB_SUCCESS) {
        free(created);
        return status;
    }
    *buffer = created;
    return MB_SUCCESS;
}

static void vk_destroy_buffer(void *self, mb_buffer *buffer)
{
    if (buffer) {
        buffer_destroy(self, buffer);
        free(buffer);
    }
}

static mb_status vk_begin_work(void *self)
{
    return jobs_begin(self);
}

static mb_status vk_submit_work(void *self)
{
    return jobs_submit(self);
}

static mb_status vk_submit_output(void *self, mb_ticket *ticket)
{
    return jobs_submit_output(self, ticket);
}

static mb_status vk_wait_idle(void *self)
{
    return jobs_wait_idle(self);
}

static const mb_backend_ops VULKAN_OPS = {
    .destroy = vk_destroy,
    .prepare = vk_prepare,
    .create_image = vk_create_image,
    .destroy_image = vk_destroy_image,
    .create_buffer = vk_create_buffer,
    .destroy_buffer = vk_destroy_buffer,
    .read_buffer = vk_read_buffer,
    .write_buffer = vk_write_buffer,
    .read_image = vk_read_image,
    .write_image = vk_write_image,
    .begin_work = vk_begin_work,
    .submit_work = vk_submit_work,
    .submit_output = vk_submit_output,
    .wait_idle = vk_wait_idle,
    .output_ready = vk_output_ready,
    .read_output = vk_read_output,
    .finish_output = vk_finish_output,
    .fill_buffer = vk_fill_buffer,
    .copy_overlap = vk_copy_overlap,
    .upload_frame = vk_upload_frame,
    .load_device_frame = vk_load_device_frame,
    .fill_from_border = vk_fill_from_border,
    .poll_bar_flags = vk_poll_bar_flags,
    .clear_bar_flags = vk_clear_bar_flags,
    .record_host_output = vk_record_host_output,
    .record_device_output = vk_record_device_output,
    .normalize = vk_normalize,
    .downsample = vk_downsample,
    .full_search = vk_full_search,
    .iterate_search = vk_iterate_search,
    .measure = vk_measure,
    .project = vk_project,
    .cross_check = vk_cross_check,
    .local_search = vk_local_search,
    .decide_cut = vk_decide_cut,
    .select = vk_select,
    .median = vk_median,
    .fallback = vk_fallback,
    .chroma_offsets = vk_chroma_offsets,
    .offset_median = vk_offset_median,
    .global_motion = vk_global_motion,
    .panels = vk_panels,
    .panel_spread = vk_panel_spread,
    .split = vk_split,
    .interpolate = vk_interpolate,
    .extrapolate = vk_extrapolate,
};

static mb_status create_sampler(mbvk_backend *vk)
{
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(vk->device, &info, NULL, &vk->sampler) == VK_SUCCESS ? MB_SUCCESS : MB_ERROR_DEVICE;
}

static mb_status create_dummy_image(mbvk_backend *vk)
{
    mb_image_desc desc = {1, 1, MB_TEXELS_R8};
    mb_status status = jobs_begin(vk);
    if (status == MB_SUCCESS) {
        status = image_create(vk, &vk->dummy_image, &desc);
    }
    if (status == MB_SUCCESS) {
        jobs_to_general(vk, &vk->dummy_image);
    }
    return status;
}

static mb_status open_backend(mbvk_backend *vk, const mbvk_config *config)
{
    mb_status status = device_open(vk, config);
    if (status == MB_SUCCESS) {
        status = jobs_create(vk);
    }
    if (status == MB_SUCCESS) {
        status = create_sampler(vk);
    }
    if (status == MB_SUCCESS) {
        status = buffer_create_device(vk, &vk->dummy, DUMMY_BYTES);
    }
    if (status == MB_SUCCESS) {
        status = create_dummy_image(vk);
    }
    if (status == MB_SUCCESS) {
        status = pipelines_create(vk);
    }
    return status;
}

mb_status mbvk_backend_create(const mbvk_config *config, mb_backend *backend, mbvk_backend **handle)
{
    mbvk_backend *vk = calloc(1, sizeof(*vk));
    if (!vk) {
        return MB_ERROR_NO_MEMORY;
    }
    vk->logger = config->logger;
    vk->prof.enabled = config->profile;
    vk->healthy = true;
    for (int k = 0; k < UPLOAD_RING; k++) {
        vk->uploads[k].slot = -1;
    }
    mb_status status = open_backend(vk, config);
    if (status != MB_SUCCESS) {
        mb_log(&vk->logger, MB_LOG_WARNING, "Vulkan init failed");
        vk_destroy(vk);
        return status;
    }
    *backend = (mb_backend){&VULKAN_OPS, vk, MAX_PENDING, (long)vk->max_groups * BLOCKS_PER_GROUP};
    if (handle) {
        *handle = vk;
    }
    return MB_SUCCESS;
}

mb_device_frame mbvk_dmabuf_frame(mbvk_dmabuf_input *input)
{
    return (mb_device_frame){MBVK_FRAME_DMABUF, input};
}

mb_device_frame mbvk_vulkan_frame_ref(mbvk_vulkan_frame *frame)
{
    return (mb_device_frame){MBVK_FRAME_VULKAN, frame};
}
