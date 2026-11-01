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

static int sample_bytes(const ref_texture *texture)
{
    return texture->format == MB_TEXELS_R8 || texture->format == MB_TEXELS_RG8 ? 1 : 2;
}

static void upload_plane(ref_texture *plane, const uint8_t *source, ptrdiff_t stride, ivec2 origin)
{
    int bytes = sample_bytes(plane);
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
    mb_image *planes[3] = {upload->planes.luma, upload->planes.chroma[0], upload->planes.chroma[1]};
    for (int i = 0; i < mb_plane_count(format); i++) {
        ivec2 origin = iv2(upload->area.x, upload->area.y);
        if (i > 0) {
            origin = iv2(upload->area.x >> format->chroma_shift_x, upload->area.y >> format->chroma_shift_y);
        }
        upload_plane(&planes[i]->texture, upload->source->planes[i], upload->source->strides[i], origin);
    }
}

static void download_plane(const ref_texture *plane, uint8_t *target, ptrdiff_t stride, ivec2 origin)
{
    int bytes = sample_bytes(plane);
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
        ivec2 origin = iv2(area.x, area.y);
        if (i > 0) {
            origin = iv2(area.x >> output->shift.x, area.y >> output->shift.y);
        }
        download_plane(&output->planes[i], target->planes[i], target->strides[i], origin);
    }
}

static bool copy_texture(ref_texture *copy, const ref_texture *source)
{
    mb_image_desc desc = {source->width, source->height, source->format};
    if (!ref_texture_alloc(copy, &desc)) {
        return false;
    }
    size_t count = (size_t)source->width * (size_t)source->height * (size_t)source->channels;
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

bool cpu_output_capture(cpu_output *output, mb_frame_planes planes, const mb_buffer *stats, ivec2 shift)
{
    cpu_output_clear(output);
    mb_image *sources[3] = {planes.luma, planes.chroma[0], planes.chroma[1]};
    for (int i = 0; i < 3 && sources[i]; i++) {
        if (!copy_texture(&output->planes[i], &sources[i]->texture)) {
            cpu_output_clear(output);
            return false;
        }
        output->plane_count = i + 1;
    }
    output->shift = shift;
    memcpy(output->stats, stats->data, sizeof(output->stats));
    return true;
}
