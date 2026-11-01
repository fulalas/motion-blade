#ifndef MBVK_INTERNAL_H
#define MBVK_INTERNAL_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "motionblade/vulkan.h"

enum {
    MAX_PENDING = MB_PENDING_LIMIT,
    SLOT_COUNT = MAX_PENDING + 1,
    MAX_TIMESTAMPS = 64,
    MAX_PROFILE = 32,
    MAX_RELEASES = 4,
    MAX_SEMAPHORES = 4,
    MAX_EXPORTS = 32,
    UPLOAD_RING = 2,
};

struct mb_image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    int width;
    int height;
    mb_texel_format format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
};

struct mb_buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    size_t bytes;
};

typedef enum pass_id {
    PASS_NORMALIZE,
    PASS_DOWNSAMPLE,
    PASS_FULL_SEARCH,
    PASS_ITERATE,
    PASS_MEASURE,
    PASS_PROJECT,
    PASS_CROSS_CHECK,
    PASS_LOCAL_SEARCH,
    PASS_DECIDE_CUT,
    PASS_SELECT,
    PASS_MEDIAN,
    PASS_FALLBACK,
    PASS_CHROMA_OFFSETS,
    PASS_OFFSET_MEDIAN,
    PASS_GLOBAL_MOTION,
    PASS_PANELS,
    PASS_PANEL_SPREAD,
    PASS_SPLIT,
    PASS_BAR_FLAGS,
    PASS_INTERPOLATE_8,
    PASS_INTERPOLATE_16,
    PASS_INTERPOLATE_8_NV,
    PASS_INTERPOLATE_16_NV,
    PASS_COUNT,
} pass_id;

typedef struct pipeline {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
} pipeline;

typedef enum output_target {
    TARGET_NONE,
    TARGET_HOST,
    TARGET_EXPORT,
    TARGET_SHARED_FRAME,
} output_target;

typedef struct plane_layout {
    size_t offsets[3];
    size_t row_bytes[3];
    size_t pixel_bytes[3];
    int rows[3];
    int count;
    size_t total;
} plane_layout;

typedef struct job_slot {
    VkCommandBuffer commands;
    VkFence fence;
    VkQueryPool queries;
    struct mb_buffer readback;
    struct mb_buffer stats_copy;
    bool busy;
    bool holds_output;
    uint32_t serial;
    output_target target;
    mb_rect area;
    int shift_x;
    int shift_y;
    plane_layout layout;
    void (*releases[MAX_RELEASES])(void *context);
    void *release_contexts[MAX_RELEASES];
    int release_count;
    struct mb_image imports[2];
    const char *timestamp_names[MAX_TIMESTAMPS];
    int timestamp_count;
    double gpu_ms;
} job_slot;

typedef struct semaphore_list {
    VkSemaphore semaphores[MAX_SEMAPHORES];
    uint64_t values[MAX_SEMAPHORES];
    int count;
} semaphore_list;

typedef struct profile {
    bool enabled;
    const char *names[MAX_PROFILE];
    double total_ms[MAX_PROFILE];
    int count;
} profile;

typedef struct staging {
    struct mb_buffer buffer;
    int slot;
    uint32_t serial;
} staging;

typedef struct background {
    struct mb_image luma;
    struct mb_image chroma;
    struct mb_buffer bar_flags;
    int bar_slot;
    uint32_t bar_serial;
    bool bars_requested;
    int flags_count;
} background;

typedef struct export_pool {
    atomic_int references;
    atomic_int busy[MAX_EXPORTS];
    atomic_uint released_at[MAX_EXPORTS];
    atomic_uint clock;
} export_pool;

struct mbvk_export_buffer {
    struct mb_buffer memory;
    int fd;
    int border_version;
    int index;
};

typedef struct exports {
    export_pool *pool;
    struct mbvk_export_buffer buffers[MAX_EXPORTS];
    int count;
    size_t offsets[3];
    size_t pitches[3];
    size_t size;
    int plane_count;
    mb_video_format format;
    int border_version;
} exports;

typedef struct device_features {
    bool dmabuf;
    bool modifiers;
    bool foreign_queue;
    bool push_descriptors;
    uint32_t subgroup_size;
    bool full_subgroups;
    uint32_t import_memory_bits;
} device_features;

struct mbvk_backend {
    mb_logger logger;
    mbvk_shared_device shared;
    bool uses_shared;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceMemoryProperties memory_properties;
    float timestamp_period;
    bool timestamps;
    uint32_t max_groups;
    device_features features;
    PFN_vkCmdPushDescriptorSetKHR push_descriptor_set;
    PFN_vkGetMemoryFdKHR get_memory_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    VkCommandPool command_pool;
    VkSampler sampler;
    pipeline pipelines[PASS_COUNT];
    struct mb_buffer dummy;
    job_slot slots[SLOT_COUNT];
    int current;
    int next_slot;
    bool recording;
    uint32_t serial;
    semaphore_list waits;
    semaphore_list signals;
    staging uploads[UPLOAD_RING];
    int next_upload;
    background bg;
    exports out;
    profile prof;
    bool healthy;
};

#endif
