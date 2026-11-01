#include "vk_memory.h"

#include <string.h>

static const VkImageSubresourceRange COLOR_RANGE = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

int memory_type(const mbvk_backend *vk, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    const VkPhysicalDeviceMemoryProperties *p = &vk->memory_properties;
    for (uint32_t i = 0; i < p->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = p->memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) {
            return (int)i;
        }
    }
    for (uint32_t i = 0; i < p->memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (p->memoryTypes[i].propertyFlags & want) == want) {
            return (int)i;
        }
    }
    return -1;
}

static int host_memory_type(const mbvk_backend *vk, uint32_t bits)
{
    VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int type = memory_type(vk, bits, visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    return type >= 0 ? type : memory_type(vk, bits, visible, 0);
}

static mb_status allocate_buffer(mbvk_backend *vk, struct mb_buffer *buffer, size_t bytes, bool host)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->bytes = bytes;
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes ? bytes : 4;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(vk->device, &info, NULL, &buffer->buffer) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->device, buffer->buffer, &req);
    int type = host ? host_memory_type(vk, req.memoryTypeBits)
                    : memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) {
        return MB_ERROR_UNSUPPORTED;
    }
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    if (vkAllocateMemory(vk->device, &alloc, NULL, &buffer->memory) != VK_SUCCESS ||
        vkBindBufferMemory(vk->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS) {
        return MB_ERROR_NO_MEMORY;
    }
    if (host && vkMapMemory(vk->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    return MB_SUCCESS;
}

mb_status buffer_create_device(mbvk_backend *vk, struct mb_buffer *buffer, size_t bytes)
{
    mb_status status = allocate_buffer(vk, buffer, bytes, false);
    if (status != MB_SUCCESS) {
        buffer_destroy(vk, buffer);
    }
    return status;
}

mb_status buffer_create_host(mbvk_backend *vk, struct mb_buffer *buffer, size_t bytes)
{
    mb_status status = allocate_buffer(vk, buffer, bytes, true);
    if (status != MB_SUCCESS) {
        buffer_destroy(vk, buffer);
    }
    return status;
}

void buffer_destroy(mbvk_backend *vk, struct mb_buffer *buffer)
{
    if (buffer->buffer) {
        vkDestroyBuffer(vk->device, buffer->buffer, NULL);
    }
    if (buffer->memory) {
        vkFreeMemory(vk->device, buffer->memory, NULL);
    }
    memset(buffer, 0, sizeof(*buffer));
}

VkFormat texel_format(mb_texel_format format)
{
    switch (format) {
    case MB_TEXELS_R8:
        return VK_FORMAT_R8_UNORM;
    case MB_TEXELS_RG8:
        return VK_FORMAT_R8G8_UNORM;
    case MB_TEXELS_R16:
        return VK_FORMAT_R16_UNORM;
    case MB_TEXELS_RG16:
        return VK_FORMAT_R16G16_UNORM;
    case MB_TEXELS_R16F:
        return VK_FORMAT_R16_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkImageCreateInfo image_info(struct mb_image *image, const mb_image_desc *desc, VkImageUsageFlags usage)
{
    memset(image, 0, sizeof(*image));
    image->width = desc->width;
    image->height = desc->height;
    image->format = desc->format;
    image->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    image->layout = VK_IMAGE_LAYOUT_GENERAL;
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = texel_format(desc->format);
    info.extent = (VkExtent3D){(uint32_t)desc->width, (uint32_t)desc->height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    return info;
}

mb_status image_view(mbvk_backend *vk, struct mb_image *image)
{
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = texel_format(image->format);
    info.subresourceRange = COLOR_RANGE;
    return vkCreateImageView(vk->device, &info, NULL, &image->view) == VK_SUCCESS ? MB_SUCCESS : MB_ERROR_DEVICE;
}

static mb_status allocate_image(mbvk_backend *vk, struct mb_image *image, const mb_image_desc *desc)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageCreateInfo info = image_info(image, desc, usage);
    if (vkCreateImage(vk->device, &info, NULL, &image->image) != VK_SUCCESS) {
        return MB_ERROR_DEVICE;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, image->image, &req);
    int type = memory_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) {
        return MB_ERROR_UNSUPPORTED;
    }
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    if (vkAllocateMemory(vk->device, &alloc, NULL, &image->memory) != VK_SUCCESS ||
        vkBindImageMemory(vk->device, image->image, image->memory, 0) != VK_SUCCESS) {
        return MB_ERROR_NO_MEMORY;
    }
    return image_view(vk, image);
}

mb_status image_create(mbvk_backend *vk, struct mb_image *image, const mb_image_desc *desc)
{
    mb_status status = allocate_image(vk, image, desc);
    if (status != MB_SUCCESS) {
        image_destroy(vk, image);
    }
    return status;
}

void image_destroy(mbvk_backend *vk, struct mb_image *image)
{
    if (image->view) {
        vkDestroyImageView(vk->device, image->view, NULL);
    }
    if (image->image) {
        vkDestroyImage(vk->device, image->image, NULL);
    }
    if (image->memory) {
        vkFreeMemory(vk->device, image->memory, NULL);
    }
    memset(image, 0, sizeof(*image));
}
