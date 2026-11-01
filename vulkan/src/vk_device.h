#ifndef MBVK_DEVICE_H
#define MBVK_DEVICE_H

#include "vk_internal.h"

mb_status device_open(mbvk_backend *vk, const mbvk_config *config);
void device_close(mbvk_backend *vk);
VkResult device_submit(mbvk_backend *vk, const VkSubmitInfo *info, VkFence fence);

#endif
