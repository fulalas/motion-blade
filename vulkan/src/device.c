#include "vk_device.h"

#include <stdlib.h>
#include <string.h>

enum { MAX_DEVICES = 16, MAX_FAMILIES = 16, MAX_EXTENSIONS = 8, PREFERRED_SUBGROUP = 32, NARROW_SUBGROUP = 16 };

typedef struct extension_support {
    bool push_descriptors;
    bool modifiers;
    bool foreign_queue;
    bool memory_fd;
    bool dmabuf;
    bool priority;
    bool subgroup_control;
} extension_support;

static int device_score(const VkPhysicalDeviceProperties *p, const char *wanted)
{
    if (wanted && wanted[0] && !strcmp(wanted, p->deviceName)) {
        return 4;
    }
    switch (p->deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 0;
    default:
        return 1;
    }
}

static mb_status pick_device(mbvk_backend *vk, const char *wanted)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "motionblade", 1, "motionblade", 1,
                             VK_API_VERSION_1_2};
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&info, NULL, &vk->instance) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    uint32_t count = MAX_DEVICES;
    VkPhysicalDevice devices[MAX_DEVICES];
    VkResult r = vkEnumeratePhysicalDevices(vk->instance, &count, devices);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        mb_log(&vk->logger, MB_LOG_ERROR, "vkEnumeratePhysicalDevices failed (%d)", r);
        return MB_ERROR_DEVICE;
    }
    int best = -1;
    int best_score = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        int score = device_score(&p, wanted);
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    if (best < 0) {
        return MB_ERROR_UNSUPPORTED;
    }
    vk->physical_device = devices[best];
    return MB_SUCCESS;
}

static bool has_compute(const VkQueueFamilyProperties *f)
{
    return (f->queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
}

static bool find_queue_family(mbvk_backend *vk)
{
    uint32_t count = MAX_FAMILIES;
    VkQueueFamilyProperties families[MAX_FAMILIES];
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &count, families);
    vk->queue_family = UINT32_MAX;
    if (vk->uses_shared && vk->shared.queue_family < count && has_compute(&families[vk->shared.queue_family])) {
        vk->queue_family = vk->shared.queue_family;
    }
    for (int pass = 0; pass < 3 && vk->queue_family == UINT32_MAX && !vk->uses_shared; pass++) {
        for (uint32_t i = 0; i < count; i++) {
            bool timed = pass == 2 || families[i].timestampValidBits;
            bool dedicated = pass || !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
            if (has_compute(&families[i]) && timed && dedicated) {
                vk->queue_family = i;
                break;
            }
        }
    }
    if (vk->queue_family != UINT32_MAX) {
        vk->timestamps = families[vk->queue_family].timestampValidBits != 0;
    }
    return vk->queue_family != UINT32_MAX;
}

static extension_support list_extensions(VkPhysicalDevice device)
{
    extension_support s = {0};
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);
    VkExtensionProperties *all = calloc(count ? count : 1, sizeof(*all));
    if (!all) {
        return s;
    }
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, all);
    for (uint32_t i = 0; i < count; i++) {
        const char *name = all[i].extensionName;
        s.push_descriptors |= !strcmp(name, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        s.modifiers |= !strcmp(name, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        s.foreign_queue |= !strcmp(name, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        s.memory_fd |= !strcmp(name, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        s.dmabuf |= !strcmp(name, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        s.priority |= !strcmp(name, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
        s.subgroup_control |= !strcmp(name, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    }
    free(all);
    return s;
}

typedef struct subgroup_info {
    VkPhysicalDeviceSubgroupSizeControlPropertiesEXT control;
    VkPhysicalDeviceSubgroupProperties basic;
} subgroup_info;

static subgroup_info query_subgroups(VkPhysicalDevice device, bool control)
{
    subgroup_info info = {{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT},
                          {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES}};
    info.basic.pNext = control ? &info.control : NULL;
    VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &info.basic};
    vkGetPhysicalDeviceProperties2(device, &properties);
    info.basic.pNext = NULL;
    return info;
}

static bool shared_subgroup_control(const mbvk_shared_device *shared, bool *full)
{
    *full = false;
    const VkBaseInStructure *b = shared->features ? shared->features->pNext : NULL;
    for (; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            const VkPhysicalDeviceVulkan13Features *f = (const void *)b;
            *full = f->computeFullSubgroups;
            return f->subgroupSizeControl;
        }
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES) {
            const VkPhysicalDeviceSubgroupSizeControlFeatures *f = (const void *)b;
            *full = f->computeFullSubgroups;
            return f->subgroupSizeControl;
        }
    }
    return false;
}

static VkPhysicalDeviceSubgroupSizeControlFeaturesEXT own_subgroup_features(VkPhysicalDevice device, bool control)
{
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT f = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT,
    };
    if (control) {
        VkPhysicalDeviceFeatures2 all = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f};
        vkGetPhysicalDeviceFeatures2(device, &all);
        f.pNext = NULL;
    }
    return f;
}

static mb_status choose_subgroup_size(mbvk_backend *vk, const extension_support *ext, const char *name)
{
    subgroup_info info = query_subgroups(vk->physical_device, ext->subgroup_control);
    VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_CLUSTERED_BIT;
    bool compute = (info.basic.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    if ((info.basic.supportedOperations & need) != need || !compute) {
        mb_log(&vk->logger, MB_LOG_WARNING, "%s lacks clustered subgroup operations", name);
        return MB_ERROR_UNSUPPORTED;
    }
    bool full = false;
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT own =
        own_subgroup_features(vk->physical_device, ext->subgroup_control);
    bool enabled = vk->uses_shared ? shared_subgroup_control(&vk->shared, &full) : own.subgroupSizeControl;
    full = vk->uses_shared ? full : own.computeFullSubgroups;
    const VkPhysicalDeviceSubgroupSizeControlPropertiesEXT *c = &info.control;
    uint32_t smallest = ext->subgroup_control ? c->minSubgroupSize : info.basic.subgroupSize;
    bool stage_ok = (c->requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    bool can_require = ext->subgroup_control && enabled && stage_ok;
    if (smallest >= NARROW_SUBGROUP && can_require && c->minSubgroupSize <= PREFERRED_SUBGROUP &&
        c->maxSubgroupSize >= PREFERRED_SUBGROUP && c->maxComputeWorkgroupSubgroups >= 2) {
        vk->features.subgroup_size = PREFERRED_SUBGROUP;
    } else if (smallest < NARROW_SUBGROUP) {
        if (!can_require || c->maxSubgroupSize < NARROW_SUBGROUP || c->maxComputeWorkgroupSubgroups < 4) {
            mb_log(&vk->logger, MB_LOG_WARNING, "%s cannot run 16-wide subgroups", name);
            return MB_ERROR_UNSUPPORTED;
        }
        vk->features.subgroup_size = NARROW_SUBGROUP;
    }
    vk->features.full_subgroups = vk->features.subgroup_size && full;
    return MB_SUCCESS;
}

typedef struct extension_list {
    const char *names[MAX_EXTENSIONS];
    uint32_t count;
} extension_list;

static void add_extension(extension_list *list, const char *name)
{
    list->names[list->count++] = name;
}

static extension_list wanted_extensions(mbvk_backend *vk, const extension_support *ext, uint32_t api)
{
    extension_list list = {.count = 0};
    add_extension(&list, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
#ifndef _WIN32
    if (ext->memory_fd && ext->dmabuf) {
        add_extension(&list, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        add_extension(&list, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        vk->features.dmabuf = true;
        if (ext->modifiers && api >= VK_API_VERSION_1_2) {
            add_extension(&list, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
            vk->features.modifiers = true;
            if (ext->foreign_queue) {
                add_extension(&list, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
                vk->features.foreign_queue = true;
            }
        }
    }
#else
    (void)ext;
    (void)api;
#endif
    if (vk->features.subgroup_size) {
        add_extension(&list, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    }
    return list;
}

static VkResult try_create(mbvk_backend *vk, VkDeviceCreateInfo *info, extension_list *list, bool low_priority)
{
    VkDeviceQueueGlobalPriorityCreateInfoKHR low = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR, NULL, VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR,
    };
    VkDeviceQueueCreateInfo *queue = (VkDeviceQueueCreateInfo *)info->pQueueCreateInfos;
    queue->pNext = low_priority ? &low : NULL;
    list->names[list->count] = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
    info->enabledExtensionCount = list->count + (low_priority ? 1 : 0);
    return vkCreateDevice(vk->physical_device, info, NULL, &vk->device);
}

static mb_status create_device(mbvk_backend *vk, const extension_support *ext, uint32_t api)
{
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, vk->queue_family, 1, &priority,
    };
    extension_list list = wanted_extensions(vk, ext, api);
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroups = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT, NULL, VK_TRUE,
        vk->features.full_subgroups ? VK_TRUE : VK_FALSE};
    VkPhysicalDeviceFeatures features = {.shaderStorageImageExtendedFormats = VK_TRUE};
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.ppEnabledExtensionNames = list.names;
    info.pEnabledFeatures = &features;
    info.pNext = vk->features.subgroup_size ? &subgroups : NULL;
    VkResult r = ext->priority ? try_create(vk, &info, &list, true) : VK_ERROR_INITIALIZATION_FAILED;
    bool low_priority = r == VK_SUCCESS;
    if (r != VK_SUCCESS) {
        r = try_create(vk, &info, &list, false);
    }
    if (r != VK_SUCCESS) {
        mb_log(&vk->logger, MB_LOG_ERROR, "vkCreateDevice failed (%d)", r);
        return MB_ERROR_DEVICE;
    }
    mb_log(&vk->logger, MB_LOG_VERBOSE, "queue family %u%s", vk->queue_family, low_priority ? ", low priority" : "");
    return MB_SUCCESS;
}

static void load_functions(mbvk_backend *vk)
{
    PFN_vkVoidFunction push = vkGetDeviceProcAddr(vk->device, "vkCmdPushDescriptorSetKHR");
    if (!push) {
        push = vkGetDeviceProcAddr(vk->device, "vkCmdPushDescriptorSet");
    }
    vk->push_descriptor_set = (PFN_vkCmdPushDescriptorSetKHR)push;
    if (vk->features.dmabuf) {
        vk->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdKHR");
        vk->get_memory_fd_properties =
            (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
        vk->features.modifiers = vk->features.modifiers && vk->get_memory_fd_properties;
    }
}

static mb_status check_device(mbvk_backend *vk, const VkPhysicalDeviceProperties *p, const extension_support *ext)
{
    if (p->apiVersion < VK_API_VERSION_1_1) {
        mb_log(&vk->logger, MB_LOG_WARNING, "%s does not support Vulkan 1.1", p->deviceName);
        return MB_ERROR_UNSUPPORTED;
    }
    if (!vk->uses_shared && !ext->push_descriptors) {
        mb_log(&vk->logger, MB_LOG_WARNING, "%s lacks push descriptors", p->deviceName);
        return MB_ERROR_UNSUPPORTED;
    }
    const VkPhysicalDeviceFeatures2 *shared_features = vk->shared.features;
    if (vk->uses_shared && (!shared_features || !shared_features->features.shaderStorageImageExtendedFormats)) {
        mb_log(&vk->logger, MB_LOG_WARNING, "the player's Vulkan device lacks shaderStorageImageExtendedFormats");
        return MB_ERROR_UNSUPPORTED;
    }
    return MB_SUCCESS;
}

static mb_status adopt_or_pick(mbvk_backend *vk, const mbvk_config *config)
{
    if (!vk->uses_shared) {
        return pick_device(vk, config->device_name);
    }
    vk->instance = vk->shared.instance;
    vk->physical_device = vk->shared.physical_device;
    vk->device = vk->shared.device;
    return MB_SUCCESS;
}

static mb_status prepare_device(mbvk_backend *vk, const VkPhysicalDeviceProperties *p)
{
    extension_support ext = list_extensions(vk->physical_device);
    mb_status status = check_device(vk, p, &ext);
    if (status == MB_SUCCESS && !find_queue_family(vk)) {
        mb_log(&vk->logger, MB_LOG_WARNING, "no queue family can run compute");
        status = MB_ERROR_UNSUPPORTED;
    }
    if (status == MB_SUCCESS) {
        status = choose_subgroup_size(vk, &ext, p->deviceName);
    }
    if (status == MB_SUCCESS && !vk->uses_shared) {
        status = create_device(vk, &ext, p->apiVersion);
    }
    if (status == MB_SUCCESS && vk->uses_shared) {
        mb_log(&vk->logger, MB_LOG_VERBOSE, "sharing the player's Vulkan device, queue family %u", vk->queue_family);
    }
    return status;
}

mb_status device_open(mbvk_backend *vk, const mbvk_config *config)
{
    vk->uses_shared = config->shared != NULL;
    if (vk->uses_shared) {
        vk->shared = *config->shared;
    }
    mb_status status = adopt_or_pick(vk, config);
    if (status != MB_SUCCESS) {
        return status;
    }
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(vk->physical_device, &p);
    vk->timestamp_period = p.limits.timestampPeriod;
    vk->max_groups = p.limits.maxComputeWorkGroupCount[0];
    vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &vk->memory_properties);
    mb_log(&vk->logger, MB_LOG_VERBOSE, "using %s", p.deviceName);
    status = prepare_device(vk, &p);
    if (status != MB_SUCCESS) {
        return status;
    }
    vkGetDeviceQueue(vk->device, vk->queue_family, 0, &vk->queue);
    load_functions(vk);
    if (!vk->push_descriptor_set) {
        mb_log(&vk->logger, MB_LOG_WARNING, "the Vulkan device lacks push descriptors");
        return MB_ERROR_UNSUPPORTED;
    }
    return MB_SUCCESS;
}

void device_close(mbvk_backend *vk)
{
    if (vk->uses_shared) {
        return;
    }
    if (vk->device) {
        vkDestroyDevice(vk->device, NULL);
    }
    if (vk->instance) {
        vkDestroyInstance(vk->instance, NULL);
    }
}

VkResult device_submit(mbvk_backend *vk, const VkSubmitInfo *info, VkFence fence)
{
    if (vk->shared.lock_queue) {
        vk->shared.lock_queue(vk->shared.context, vk->queue_family, 0);
    }
    VkResult r = vkQueueSubmit(vk->queue, 1, info, fence);
    if (vk->shared.unlock_queue) {
        vk->shared.unlock_queue(vk->shared.context, vk->queue_family, 0);
    }
    return r;
}
