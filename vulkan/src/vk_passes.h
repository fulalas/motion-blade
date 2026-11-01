#ifndef MBVK_PASSES_H
#define MBVK_PASSES_H

#include "vk_internal.h"

void vk_normalize(void *self, const mb_normalize_pass *pass);
void vk_downsample(void *self, const mb_downsample_pass *pass);
void vk_full_search(void *self, const mb_full_search_pass *pass);
void vk_iterate_search(void *self, const mb_iterate_pass *pass);
void vk_measure(void *self, const mb_measure_pass *pass);
void vk_project(void *self, const mb_project_pass *pass);
void vk_cross_check(void *self, const mb_cross_check_pass *pass);
void vk_local_search(void *self, const mb_local_search_pass *pass);
void vk_decide_cut(void *self, const mb_cut_pass *pass);
void vk_select(void *self, const mb_select_pass *pass);
void vk_median(void *self, const mb_median_pass *pass);
void vk_fallback(void *self, const mb_fallback_pass *pass);
void vk_chroma_offsets(void *self, const mb_chroma_offset_pass *pass);
void vk_offset_median(void *self, const mb_offset_median_pass *pass);
void vk_global_motion(void *self, const mb_global_motion_pass *pass);
void vk_panels(void *self, const mb_panel_pass *pass);
void vk_panel_spread(void *self, const mb_panel_spread_pass *pass);
void vk_split(void *self, const mb_split_pass *pass);
void vk_interpolate(void *self, const mb_interpolate_pass *pass);

#endif
