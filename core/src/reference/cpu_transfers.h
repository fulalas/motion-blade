#ifndef MB_CPU_TRANSFERS_H
#define MB_CPU_TRANSFERS_H

#include "motionblade/backend.h"
#include "reference/cpu_resources.h"

typedef struct cpu_output {
    ref_texture planes[3];
    int plane_count;
    ivec2 shift;
    uint32_t stats[MB_STAT_COUNT];
    bool in_use;
    uint32_t serial;
} cpu_output;

void cpu_upload_frame(const mb_upload *upload);
void cpu_download_planes(const cpu_output *output, const mb_host_image *target, mb_rect area);
bool cpu_output_capture(cpu_output *output, mb_frame_planes planes, const mb_buffer *stats, ivec2 shift);
void cpu_output_clear(cpu_output *output);

#endif
