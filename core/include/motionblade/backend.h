#ifndef MOTIONBLADE_BACKEND_H
#define MOTIONBLADE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "motionblade/layout.h"
#include "motionblade/passes.h"
#include "motionblade/resources.h"
#include "motionblade/status.h"
#include "motionblade/video.h"

typedef struct mb_ticket {
    int slot;
    uint32_t serial;
} mb_ticket;

typedef struct mb_device_frame {
    int kind;
    void *payload;
} mb_device_frame;

enum { MB_BORDER_FROM_FRAME = -2 };

typedef struct mb_border {
    const mb_host_image *host_template;
    int version;
} mb_border;

typedef struct mb_upload {
    mb_frame_planes planes;
    const mb_host_image *source;
    mb_rect area;
    const mb_video_format *format;
} mb_upload;

typedef struct mb_device_load {
    mb_frame_planes planes;
    const mb_device_frame *frame;
    const mb_video_format *format;
    mb_rect area;
    bool refresh_border;
    bool detect_bars;
} mb_device_load;

typedef struct mb_overlap_copy {
    mb_image *source;
    mb_image *target;
    int source_x;
    int source_y;
    int target_x;
    int target_y;
} mb_overlap_copy;

typedef struct mb_output_copy {
    mb_frame_planes planes;
    mb_rect area;
    const mb_video_format *format;
    const mb_device_frame *target;
    mb_border border;
} mb_output_copy;

typedef struct mb_output_stats {
    bool valid;
    bool scene_cut;
    uint32_t error_sum;
    double gpu_ms;
} mb_output_stats;

typedef struct mb_backend_ops {
    void (*destroy)(void *self);
    void (*prepare)(void *self, const mb_video_format *format);
    mb_status (*create_image)(void *self, const mb_image_desc *desc, mb_image **image);
    void (*destroy_image)(void *self, mb_image *image);
    mb_status (*create_buffer)(void *self, size_t bytes, mb_buffer **buffer);
    void (*destroy_buffer)(void *self, mb_buffer *buffer);
    mb_status (*read_buffer)(void *self, mb_buffer *buffer, void *data, size_t bytes);
    mb_status (*write_buffer)(void *self, mb_buffer *buffer, const void *data, size_t bytes);
    mb_status (*read_image)(void *self, mb_image *image, float *texels);
    mb_status (*write_image)(void *self, mb_image *image, const float *texels);

    mb_status (*begin_work)(void *self);
    mb_status (*submit_work)(void *self);
    mb_status (*submit_output)(void *self, mb_ticket *ticket);
    mb_status (*wait_idle)(void *self);
    bool (*output_ready)(void *self, mb_ticket ticket);
    mb_status (*read_output)(void *self, mb_ticket ticket, const mb_host_image *target, mb_rect area);
    mb_status (*finish_output)(void *self, mb_ticket ticket, mb_output_stats *stats);

    void (*fill_buffer)(void *self, mb_buffer *buffer, uint32_t value);
    void (*copy_overlap)(void *self, const mb_overlap_copy *copy);
    mb_status (*upload_frame)(void *self, const mb_upload *upload);
    mb_status (*load_device_frame)(void *self, const mb_device_load *load);
    void (*fill_from_border)(void *self, mb_frame_planes planes, mb_rect area, const mb_video_format *format);
    bool (*poll_bar_flags)(void *self, const uint32_t **flags);
    void (*clear_bar_flags)(void *self);
    mb_status (*record_host_output)(void *self, const mb_output_copy *copy, mb_buffer *stats);
    mb_status (*record_device_output)(void *self, const mb_output_copy *copy, mb_buffer *stats);

    void (*normalize)(void *self, const mb_normalize_pass *pass);
    void (*downsample)(void *self, const mb_downsample_pass *pass);
    void (*full_search)(void *self, const mb_full_search_pass *pass);
    void (*iterate_search)(void *self, const mb_iterate_pass *pass);
    void (*measure)(void *self, const mb_measure_pass *pass);
    void (*project)(void *self, const mb_project_pass *pass);
    void (*cross_check)(void *self, const mb_cross_check_pass *pass);
    void (*local_search)(void *self, const mb_local_search_pass *pass);
    void (*decide_cut)(void *self, const mb_cut_pass *pass);
    void (*select)(void *self, const mb_select_pass *pass);
    void (*median)(void *self, const mb_median_pass *pass);
    void (*fallback)(void *self, const mb_fallback_pass *pass);
    void (*chroma_offsets)(void *self, const mb_chroma_offset_pass *pass);
    void (*offset_median)(void *self, const mb_offset_median_pass *pass);
    void (*global_motion)(void *self, const mb_global_motion_pass *pass);
    void (*panels)(void *self, const mb_panel_pass *pass);
    void (*panel_spread)(void *self, const mb_panel_spread_pass *pass);
    void (*split)(void *self, const mb_split_pass *pass);
    void (*interpolate)(void *self, const mb_interpolate_pass *pass);
    void (*extrapolate)(void *self, const mb_extrapolate_pass *pass);
} mb_backend_ops;

typedef struct mb_backend {
    const mb_backend_ops *ops;
    void *self;
    int max_pending;
    long max_blocks;
} mb_backend;

bool mb_backend_is_complete(const mb_backend *backend);

#endif
