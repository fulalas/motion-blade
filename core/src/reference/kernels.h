#ifndef MB_REFERENCE_KERNELS_H
#define MB_REFERENCE_KERNELS_H

#include "motionblade/passes.h"

void ref_normalize(const mb_normalize_pass *pass);
void ref_downsample(const mb_downsample_pass *pass);
void ref_full_search(const mb_full_search_pass *pass);
void ref_iterate_search(const mb_iterate_pass *pass);
void ref_measure(const mb_measure_pass *pass);
void ref_project(const mb_project_pass *pass);
void ref_cross_check(const mb_cross_check_pass *pass);
void ref_local_search(const mb_local_search_pass *pass);
void ref_decide_cut(const mb_cut_pass *pass);
void ref_select(const mb_select_pass *pass);
void ref_median(const mb_median_pass *pass);
void ref_fallback(const mb_fallback_pass *pass);
void ref_chroma_offsets(const mb_chroma_offset_pass *pass);
void ref_offset_median(const mb_offset_median_pass *pass);
void ref_global_motion(const mb_global_motion_pass *pass);
void ref_panels(const mb_panel_pass *pass);
void ref_panel_spread(const mb_panel_spread_pass *pass);
void ref_split(const mb_split_pass *pass);
void ref_interpolate(const mb_interpolate_pass *pass);

#endif
