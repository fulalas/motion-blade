#ifndef MBVK_MEMORY_H
#define MBVK_MEMORY_H

#include "vk_internal.h"

int memory_type(const mbvk_backend *vk, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid);
mb_status buffer_create_device(mbvk_backend *vk, struct mb_buffer *buffer, size_t bytes);
mb_status buffer_create_host(mbvk_backend *vk, struct mb_buffer *buffer, size_t bytes);
void buffer_destroy(mbvk_backend *vk, struct mb_buffer *buffer);
VkFormat texel_format(mb_texel_format format);
VkImageCreateInfo image_info(struct mb_image *image, const mb_image_desc *desc, VkImageUsageFlags usage);
mb_status image_view(mbvk_backend *vk, struct mb_image *image);
mb_status image_create(mbvk_backend *vk, struct mb_image *image, const mb_image_desc *desc);
void image_destroy(mbvk_backend *vk, struct mb_image *image);

#endif
