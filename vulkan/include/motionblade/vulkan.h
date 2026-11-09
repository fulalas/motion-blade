#ifndef MOTIONBLADE_VULKAN_H
#define MOTIONBLADE_VULKAN_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "motionblade/backend.h"
#include "motionblade/log.h"

typedef struct mbvk_backend mbvk_backend;

typedef struct mbvk_shared_device {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family;
    const VkPhysicalDeviceFeatures2 *features;
    void (*lock_queue)(void *context, uint32_t family, uint32_t index);
    void (*unlock_queue)(void *context, uint32_t family, uint32_t index);
    void *context;
} mbvk_shared_device;

typedef struct mbvk_config {
    const mbvk_shared_device *shared;
    const char *device_name;
    mb_logger logger;
    bool profile;
} mbvk_config;

typedef enum mbvk_frame_kind {
    MBVK_FRAME_DMABUF = 1,
    MBVK_FRAME_VULKAN,
    MBVK_FRAME_EXPORT,
} mbvk_frame_kind;

typedef struct mbvk_dmabuf_input {
    int fds[4];
    uint64_t modifier;
    int objects[2];
    uint32_t offsets[2];
    uint32_t pitches[2];
    void (*release)(void *context);
    void *context;
} mbvk_dmabuf_input;

typedef struct mbvk_vulkan_frame {
    VkImage images[2];
    int image_count;
    VkImageLayout layouts[2];
    VkSemaphore semaphores[2];
    uint64_t semaphore_values[2];
    uint32_t queue_families[2];
} mbvk_vulkan_frame;

typedef struct mbvk_export_buffer mbvk_export_buffer;

typedef struct mbvk_dmabuf_output {
    int fd;
    int plane_count;
    uint32_t offsets[3];
    uint32_t pitches[3];
    uint64_t size;
    void *pool;
    int index;
} mbvk_dmabuf_output;

mb_status mbvk_backend_create(const mbvk_config *config, mb_backend *backend, mbvk_backend **handle);
mb_device_frame mbvk_dmabuf_frame(mbvk_dmabuf_input *input);
mb_device_frame mbvk_vulkan_frame_ref(mbvk_vulkan_frame *frame);

mb_status mbvk_export_enable(mbvk_backend *vk, const mb_video_format *format);
bool mbvk_export_trusted(const mbvk_backend *vk);
mb_status mbvk_export_acquire(mbvk_backend *vk, mbvk_export_buffer **buffer);
mb_status mbvk_export_host_image(mbvk_backend *vk, const mb_host_image *source, mbvk_export_buffer **buffer);
mb_device_frame mbvk_export_target(mbvk_export_buffer *buffer);
mb_status mbvk_export_describe(mbvk_backend *vk, mbvk_export_buffer *buffer, mbvk_dmabuf_output *output);
void mbvk_export_cancel(mbvk_backend *vk, mbvk_export_buffer *buffer);
void mbvk_export_read(mbvk_backend *vk, const mbvk_export_buffer *buffer, const mb_host_image *target);
void mbvk_export_release(void *pool, int index);

#endif
