#include "vk_jobs.h"

#include <stdio.h>
#include <string.h>

#include "vk_device.h"
#include "vk_memory.h"

enum { WAIT_ATTEMPTS = 5, FRAME_TIMESTAMPS = 2 };

static const uint64_t WAIT_NANOSECONDS = 1000000000ull;

mb_status jobs_create(mbvk_backend *vk)
{
    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk->queue_family};
    if (vkCreateCommandPool(vk->device, &pool, NULL, &vk->command_pool) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkCommandBufferAllocateInfo commands = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, vk->command_pool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo queries = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP,
                                     MAX_TIMESTAMPS + FRAME_TIMESTAMPS, 0};
    for (int i = 0; i < SLOT_COUNT; i++) {
        job_slot *s = &vk->slots[i];
        if (vkAllocateCommandBuffers(vk->device, &commands, &s->commands) != VK_SUCCESS ||
            vkCreateFence(vk->device, &fence, NULL, &s->fence) != VK_SUCCESS ||
            vkCreateQueryPool(vk->device, &queries, NULL, &s->queries) != VK_SUCCESS) {
            return MB_ERROR_DEVICE;
        }
        if (buffer_create_host(vk, &s->stats_copy, MB_STAT_COUNT * sizeof(uint32_t)) != MB_SUCCESS) {
            return MB_ERROR_NO_MEMORY;
        }
    }
    return MB_SUCCESS;
}

static void release_slot(mbvk_backend *vk, job_slot *s)
{
    for (int k = 0; k < 2; k++) {
        image_destroy(vk, &s->imports[k]);
    }
    for (int k = 0; k < s->release_count; k++) {
        s->releases[k](s->release_contexts[k]);
    }
    s->release_count = 0;
}

void jobs_destroy(mbvk_backend *vk)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        job_slot *s = &vk->slots[i];
        release_slot(vk, s);
        buffer_destroy(vk, &s->readback);
        buffer_destroy(vk, &s->stats_copy);
        if (s->queries) {
            vkDestroyQueryPool(vk->device, s->queries, NULL);
        }
        if (s->fence) {
            vkDestroyFence(vk->device, s->fence, NULL);
        }
    }
    if (vk->command_pool) {
        vkDestroyCommandPool(vk->device, vk->command_pool, NULL);
    }
}

static int profile_entry(profile *p, const char *name)
{
    for (int j = 0; j < p->count; j++) {
        if (!strcmp(p->names[j], name)) {
            return j;
        }
    }
    if (p->count == MAX_PROFILE) {
        return -1;
    }
    p->names[p->count] = name;
    return p->count++;
}

static void collect_timestamps(mbvk_backend *vk, job_slot *s)
{
    uint64_t stamps[MAX_TIMESTAMPS + FRAME_TIMESTAMPS];
    uint32_t count = (uint32_t)(FRAME_TIMESTAMPS + s->timestamp_count);
    if (!vk->timestamps || vkGetQueryPoolResults(vk->device, s->queries, 0, count, sizeof(stamps), stamps,
                                                 sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    uint64_t previous = stamps[0];
    for (int k = 0; k < s->timestamp_count; k++) {
        double ms = (double)(stamps[FRAME_TIMESTAMPS + k] - previous) * vk->timestamp_period / 1e6;
        previous = stamps[FRAME_TIMESTAMPS + k];
        int j = profile_entry(&vk->prof, s->timestamp_names[k]);
        if (j >= 0) {
            vk->prof.total_ms[j] += ms;
        }
    }
    s->gpu_ms = (double)(stamps[1] - stamps[0]) * vk->timestamp_period / 1e6;
}

mb_status jobs_wait_slot(mbvk_backend *vk, int slot)
{
    job_slot *s = &vk->slots[slot];
    if (!s->busy) {
        return MB_SUCCESS;
    }
    VkResult r = VK_TIMEOUT;
    for (int attempt = 0; attempt < WAIT_ATTEMPTS && r == VK_TIMEOUT; attempt++) {
        r = vkWaitForFences(vk->device, 1, &s->fence, VK_TRUE, WAIT_NANOSECONDS);
    }
    s->busy = false;
    release_slot(vk, s);
    s->gpu_ms = 0;
    if (r != VK_SUCCESS) {
        mb_log(&vk->logger, MB_LOG_ERROR, "waiting for the GPU failed (%d)", r);
        vk->healthy = false;
        return MB_ERROR_DEVICE;
    }
    vkResetFences(vk->device, 1, &s->fence);
    collect_timestamps(vk, s);
    return MB_SUCCESS;
}

mb_status jobs_wait_idle(mbvk_backend *vk)
{
    mb_status result = MB_SUCCESS;
    for (int i = 0; i < SLOT_COUNT; i++) {
        mb_status status = jobs_wait_slot(vk, i);
        result = result == MB_SUCCESS ? status : result;
    }
    return result;
}

void jobs_poll(mbvk_backend *vk)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (vk->slots[i].busy && vkGetFenceStatus(vk->device, vk->slots[i].fence) == VK_SUCCESS) {
            jobs_wait_slot(vk, i);
        }
    }
}

static int free_slot(const mbvk_backend *vk)
{
    for (int k = 0; k < SLOT_COUNT; k++) {
        int i = (vk->next_slot + k) % SLOT_COUNT;
        if (!vk->slots[i].holds_output) {
            return i;
        }
    }
    return -1;
}

static void start_recording(mbvk_backend *vk, job_slot *s)
{
    if (vk->timestamps) {
        vkCmdResetQueryPool(s->commands, s->queries, 0, MAX_TIMESTAMPS + FRAME_TIMESTAMPS);
        vkCmdWriteTimestamp(s->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s->queries, 0);
    }
    s->timestamp_count = 0;
    s->target = TARGET_NONE;
    vk->pending = (hazards){.read_count = 0};
    VkMemoryBarrier all = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    vkCmdPipelineBarrier(s->commands, stages, stages, 0, 1, &all, 0, NULL, 0, NULL);
}

mb_status jobs_begin(mbvk_backend *vk)
{
    if (vk->recording) {
        return MB_SUCCESS;
    }
    int i = free_slot(vk);
    if (i < 0) {
        return MB_ERROR_BUSY;
    }
    if (jobs_wait_slot(vk, i) != MB_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    job_slot *s = &vk->slots[i];
    if (vkResetCommandBuffer(s->commands, 0) != VK_SUCCESS || vkBeginCommandBuffer(s->commands, &begin) != VK_SUCCESS) {
        mb_log(&vk->logger, MB_LOG_ERROR, "could not begin command buffer");
        vk->healthy = false;
        return MB_ERROR_DEVICE;
    }
    vk->current = i;
    vk->next_slot = (i + 1) % SLOT_COUNT;
    vk->recording = true;
    start_recording(vk, s);
    return MB_SUCCESS;
}

static VkResult submit_commands(mbvk_backend *vk, job_slot *s)
{
    VkPipelineStageFlags stages[MAX_SEMAPHORES];
    for (int i = 0; i < vk->waits.count; i++) {
        stages[i] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    VkTimelineSemaphoreSubmitInfo timeline = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL,
                                              (uint32_t)vk->waits.count, vk->waits.values,
                                              (uint32_t)vk->signals.count, vk->signals.values};
    VkSubmitInfo info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &s->commands;
    if (vk->waits.count || vk->signals.count) {
        info.pNext = &timeline;
        info.waitSemaphoreCount = (uint32_t)vk->waits.count;
        info.pWaitSemaphores = vk->waits.semaphores;
        info.pWaitDstStageMask = stages;
        info.signalSemaphoreCount = (uint32_t)vk->signals.count;
        info.pSignalSemaphores = vk->signals.semaphores;
    }
    vk->waits.count = 0;
    vk->signals.count = 0;
    VkResult r = vkEndCommandBuffer(s->commands);
    return r == VK_SUCCESS ? device_submit(vk, &info, s->fence) : r;
}

static mb_status finish_recording(mbvk_backend *vk, bool holds_output)
{
    if (!vk->recording) {
        return MB_SUCCESS;
    }
    vk->recording = false;
    job_slot *s = &vk->slots[vk->current];
    if (vk->timestamps) {
        vkCmdWriteTimestamp(s->commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->queries, 1);
    }
    if (submit_commands(vk, s) != VK_SUCCESS) {
        mb_log(&vk->logger, MB_LOG_ERROR, "could not submit command buffer");
        vk->healthy = false;
        return MB_ERROR_DEVICE;
    }
    s->busy = true;
    s->serial = ++vk->serial;
    s->holds_output = holds_output;
    return MB_SUCCESS;
}

mb_status jobs_submit(mbvk_backend *vk)
{
    return finish_recording(vk, false);
}

mb_status jobs_submit_output(mbvk_backend *vk, mb_ticket *ticket)
{
    if (!vk->recording) {
        return MB_ERROR_INVALID;
    }
    int slot = vk->current;
    mb_status status = finish_recording(vk, true);
    *ticket = (mb_ticket){slot, vk->slots[slot].serial};
    return status;
}

job_slot *jobs_slot_of(mbvk_backend *vk, mb_ticket ticket)
{
    if (ticket.slot < 0 || ticket.slot >= SLOT_COUNT) {
        return NULL;
    }
    job_slot *s = &vk->slots[ticket.slot];
    return s->holds_output && s->serial == ticket.serial ? s : NULL;
}

job_slot *jobs_current(mbvk_backend *vk)
{
    return &vk->slots[vk->current];
}

VkCommandBuffer jobs_tracked_commands(mbvk_backend *vk)
{
    return vk->slots[vk->current].commands;
}

void jobs_barrier(mbvk_backend *vk)
{
    VkAccessFlags writes = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkAccessFlags reads = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, writes, reads | writes};
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(jobs_tracked_commands(vk), stages, stages, 0, 1, &b, 0, NULL, 0, NULL);
    vk->pending = (hazards){.read_count = 0};
}

VkCommandBuffer jobs_commands(mbvk_backend *vk)
{
    if (vk->pending.read_count || vk->pending.write_count) {
        jobs_barrier(vk);
    }
    vk->pending.untracked = true;
    return jobs_tracked_commands(vk);
}

static bool listed(const uint64_t *list, int count, uint64_t handle)
{
    for (int i = 0; i < count; i++) {
        if (list[i] == handle) {
            return true;
        }
    }
    return false;
}

static bool conflicts(const hazards *h, resource_use use)
{
    return listed(h->writes, h->write_count, use.handle) || (use.write && listed(h->reads, h->read_count, use.handle));
}

static bool must_wait(const hazards *h, const resource_use *uses, int count)
{
    if (h->untracked || h->read_count + count > MAX_TRACKED || h->write_count + count > MAX_TRACKED) {
        return true;
    }
    for (int i = 0; i < count; i++) {
        if (conflicts(h, uses[i])) {
            return true;
        }
    }
    return false;
}

void jobs_prepare(mbvk_backend *vk, const resource_use *uses, int count)
{
    hazards *h = &vk->pending;
    if (must_wait(h, uses, count)) {
        jobs_barrier(vk);
    }
    for (int i = 0; i < count; i++) {
        if (uses[i].write) {
            h->writes[h->write_count++] = uses[i].handle;
        } else {
            h->reads[h->read_count++] = uses[i].handle;
        }
    }
}

static uint64_t handle_bits(const void *handle, size_t size)
{
    uint64_t bits = 0;
    memcpy(&bits, handle, size);
    return bits;
}

uint64_t buffer_handle(const struct mb_buffer *buffer)
{
    return handle_bits(&buffer->buffer, sizeof(buffer->buffer));
}

uint64_t image_handle(const struct mb_image *image)
{
    return handle_bits(&image->image, sizeof(image->image));
}

void jobs_host_barrier(mbvk_backend *vk)
{
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(jobs_commands(vk), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0,
                         NULL, 0, NULL);
}

void jobs_timestamp(mbvk_backend *vk, const char *name)
{
    job_slot *s = jobs_current(vk);
    if (vk->prof.enabled && vk->timestamps && s->timestamp_count < MAX_TIMESTAMPS - 1) {
        s->timestamp_names[s->timestamp_count] = name;
        vkCmdWriteTimestamp(s->commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s->queries,
                            (uint32_t)(FRAME_TIMESTAMPS + s->timestamp_count++));
    }
}

VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = COLOR_RANGE;
    return b;
}

void jobs_to_general(mbvk_backend *vk, struct mb_image *image)
{
    VkImageMemoryBarrier b = image_barrier(image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(jobs_commands(vk), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         NULL, 0, NULL, 1, &b);
}

void jobs_add_release(mbvk_backend *vk, void (*release)(void *context), void *context)
{
    job_slot *s = jobs_current(vk);
    if (s->release_count == MAX_RELEASES) {
        release(context);
        return;
    }
    s->releases[s->release_count] = release;
    s->release_contexts[s->release_count++] = context;
}

void jobs_print_profile(const mbvk_backend *vk)
{
    for (int k = 0; k < vk->prof.count; k++) {
        fprintf(stderr, "motionblade profile: %-10s %8.1f ms total\n", vk->prof.names[k], vk->prof.total_ms[k]);
    }
}
