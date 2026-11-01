#ifndef MBVK_JOBS_H
#define MBVK_JOBS_H

#include "vk_internal.h"

mb_status jobs_create(mbvk_backend *vk);
void jobs_destroy(mbvk_backend *vk);
mb_status jobs_begin(mbvk_backend *vk);
mb_status jobs_submit(mbvk_backend *vk);
mb_status jobs_submit_output(mbvk_backend *vk, mb_ticket *ticket);
mb_status jobs_wait_slot(mbvk_backend *vk, int slot);
mb_status jobs_wait_idle(mbvk_backend *vk);
void jobs_poll(mbvk_backend *vk);
job_slot *jobs_slot_of(mbvk_backend *vk, mb_ticket ticket);
job_slot *jobs_current(mbvk_backend *vk);
VkCommandBuffer jobs_commands(mbvk_backend *vk);
void jobs_barrier(mbvk_backend *vk);
void jobs_host_barrier(mbvk_backend *vk);
void jobs_timestamp(mbvk_backend *vk, const char *name);
void jobs_to_general(mbvk_backend *vk, struct mb_image *image);
void jobs_add_release(mbvk_backend *vk, void (*release)(void *context), void *context);
void jobs_print_profile(const mbvk_backend *vk);

#endif
