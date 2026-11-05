#ifndef MBVK_EXPORT_H
#define MBVK_EXPORT_H

#include "vk_internal.h"

mb_status export_record_output(mbvk_backend *vk, const mb_output_copy *copy);
void exports_destroy(mbvk_backend *vk);

#endif
