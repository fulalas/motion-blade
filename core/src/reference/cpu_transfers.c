#include "reference/cpu_transfers.h"

#include <string.h>

static float read_sample(const uint8_t *row, int index, int bytes)
{
    if (bytes == 1) {
        return (float)row[index] / 255.0f;
    }
    uint16_t value;
    memcpy(&value, row + (size_t)index * 2, sizeof(value));
    return (float)value / 65535.0f;
}

static void write_sample(uint8_t *row, int index, int bytes, float value)
{
    if (bytes == 1) {
        row[index] = (uint8_t)lrintf(value * 255.0f);
        return;
    }
    uint16_t stored = (uint16_t)lrintf(value * 65535.0f);
    memcpy(row + (size_t)index * 2, &stored, sizeof(stored));
}

static void upload_plane(ref_texture *plane, const uint8_t *source, ptrdiff_t stride, ivec2 origin)
{
    int bytes = mb_texel_sample_bytes(plane->format);
    int span = plane->width * plane->channels;
    for (int y = 0; y < plane->height; y++) {
        const uint8_t *row = source + (ptrdiff_t)(origin.y + y) * stride;
        float *texels = plane->texels + (size_t)y * (size_t)span;
        for (int i = 0; i < span; i++) {
            texels[i] = read_sample(row, origin.x * plane->channels + i, bytes);
        }
    }
}

void cpu_upload_frame(const mb_upload *upload)
{
    const mb_video_format *format = upload->format;
    for (int i = 0; i < mb_plane_count(format); i++) {
        mb_rect area = mb_plane_area(format, i, upload->area);
        upload_plane(&mb_frame_plane(upload->planes, i)->texture, upload->source->planes[i], upload->source->strides[i],
                     iv2(area.x, area.y));
    }
}

static void download_plane(const ref_texture *plane, uint8_t *target, ptrdiff_t stride, ivec2 origin)
{
    int bytes = mb_texel_sample_bytes(plane->format);
    int span = plane->width * plane->channels;
    for (int y = 0; y < plane->height; y++) {
        uint8_t *row = target + (ptrdiff_t)(origin.y + y) * stride;
        const float *texels = plane->texels + (size_t)y * (size_t)span;
        for (int i = 0; i < span; i++) {
            write_sample(row, origin.x * plane->channels + i, bytes, texels[i]);
        }
    }
}

void cpu_download_planes(const cpu_output *output, const mb_host_image *target, mb_rect area)
{
    for (int i = 0; i < output->plane_count; i++) {
        mb_rect plane = mb_plane_area(&output->format, i, area);
        download_plane(&output->planes[i], target->planes[i], target->strides[i], iv2(plane.x, plane.y));
    }
}

static bool copy_texture(ref_texture *copy, const ref_texture *source)
{
    mb_image_desc desc = {source->width, source->height, source->format};
    if (!ref_texture_alloc(copy, &desc)) {
        return false;
    }
    size_t count = mb_texel_count(desc);
    memcpy(copy->texels, source->texels, count * sizeof(float));
    return true;
}

void cpu_output_clear(cpu_output *output)
{
    for (int i = 0; i < output->plane_count; i++) {
        ref_texture_free(&output->planes[i]);
    }
    output->plane_count = 0;
}

bool cpu_output_capture(cpu_output *output, mb_frame_planes planes, const mb_buffer *stats,
                        const mb_video_format *format)
{
    cpu_output_clear(output);
    for (int i = 0; i < 3 && mb_frame_plane(planes, i); i++) {
        if (!copy_texture(&output->planes[i], &mb_frame_plane(planes, i)->texture)) {
            cpu_output_clear(output);
            return false;
        }
        output->plane_count = i + 1;
    }
    output->format = *format;
    memcpy(output->stats, stats->data, sizeof(output->stats));
    return true;
}
