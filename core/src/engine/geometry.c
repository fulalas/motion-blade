#include "engine/geometry.h"

static const long HALF_RESOLUTION_PIXELS = 2500000L;

static int divide_up(int value, int shift)
{
    return (value + (1 << shift) - 1) >> shift;
}

static int count_levels(int width, int height)
{
    int levels = 1;
    while (levels < MB_MAX_LEVELS && (width >> levels) >= MB_MIN_LEVEL_SIZE &&
           (height >> levels) >= MB_MIN_LEVEL_SIZE) {
        levels++;
    }
    return levels;
}

static bool wants_half_resolution(const mb_video_format *source, const mb_search_params *search)
{
    if (search->half_resolution < 0) {
        return (long)source->width * source->height > HALF_RESOLUTION_PIXELS;
    }
    return search->half_resolution != 0;
}

static mb_grid level_grid(int width, int height, int level)
{
    int w = divide_up(width, level);
    int h = divide_up(height, level);
    return (mb_grid){(w + MB_BLOCK_SIZE - 1) / MB_BLOCK_SIZE, (h + MB_BLOCK_SIZE - 1) / MB_BLOCK_SIZE, w, h};
}

geometry geometry_for(const mb_video_format *source, mb_rect active, const mb_search_params *search)
{
    geometry g = {.active = active, .format = *source};
    g.format.width = active.width;
    g.format.height = active.height;
    g.chroma = (mb_chroma_layout){mb_chroma_width(&g.format), mb_chroma_height(&g.format), source->chroma_shift_x,
                                  source->chroma_shift_y, source->interleaved_chroma};
    g.level_count = count_levels(active.width, active.height);
    g.base_level = wants_half_resolution(source, search) && g.level_count > 2 ? 1 : 0;
    g.normalized_luma = mb_bytes_per_sample(source) == 2 && source->bit_depth < 16;
    for (int level = 0; level < g.level_count; level++) {
        g.grids[level] = level_grid(active.width, active.height, level);
    }
    return g;
}

mb_grid geometry_base_grid(const geometry *g)
{
    return g->grids[g->base_level];
}

int geometry_base_blocks(const geometry *g)
{
    return mb_grid_block_count(geometry_base_grid(g));
}

bool geometry_is_cropped(const geometry *g, const mb_video_format *source)
{
    return !mb_rect_equal(g->active, (mb_rect){0, 0, source->width, source->height});
}

float geometry_luma_scale(const geometry *g)
{
    return g->normalized_luma ? (float)(65535.0 / ((1 << g->format.bit_depth) - 1)) : 1.0f;
}

mb_output_range geometry_output_range(const geometry *g)
{
    const mb_video_format *f = &g->format;
    if (mb_bytes_per_sample(f) == 1) {
        return (mb_output_range){false, 1.0f, 0.0f};
    }
    int pad = f->lsb_padding;
    float max_value = g->normalized_luma ? (float)(((1 << f->bit_depth) - 1) / 65535.0)
                                         : (float)((65535 - ((1 << pad) - 1)) / 65535.0);
    float step = pad ? (float)((1 << pad) / 65535.0) : 0.0f;
    return (mb_output_range){true, max_value, step};
}

float geometry_chroma_mid(const geometry *g)
{
    float mid = (float)(1u << (g->format.bit_depth - 1));
    return mid / (mb_bytes_per_sample(&g->format) == 1 ? 255.0f : 65535.0f);
}

mb_texel_format geometry_luma_texels(const geometry *g)
{
    return mb_bytes_per_sample(&g->format) == 2 ? MB_TEXELS_R16 : MB_TEXELS_R8;
}

mb_texel_format geometry_chroma_texels(const geometry *g)
{
    if (!g->chroma.interleaved) {
        return geometry_luma_texels(g);
    }
    return mb_bytes_per_sample(&g->format) == 2 ? MB_TEXELS_RG16 : MB_TEXELS_RG8;
}
