#include "reference/cpu_resources.h"
#include "reference/kernels.h"

void ref_normalize(const mb_normalize_pass *pass)
{
    const ref_texture *source = &pass->source->texture;
    for (int y = 0; y < pass->height; y++) {
        for (int x = 0; x < pass->width; x++) {
            float value = ref_texel(source, x, y, 0) * pass->scale;
            ref_store(&pass->target->texture, x, y, 0, value < 1.0f ? value : 1.0f);
        }
    }
}

void ref_downsample(const mb_downsample_pass *pass)
{
    const ref_texture *source = &pass->source->texture;
    for (int y = 0; y < pass->height; y++) {
        for (int x = 0; x < pass->width; x++) {
            float u = ((float)x * 2.0f + 1.0f) / (float)pass->source_width;
            float v = ((float)y * 2.0f + 1.0f) / (float)pass->source_height;
            ref_store(&pass->target->texture, x, y, 0, ref_sample(source, u, v, 0));
        }
    }
}
