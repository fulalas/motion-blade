#include <math.h>

#include "common/blocks.h"
#include "estimation/window.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

static const float STAT_UNIT = (float)(1.0 / MB_STAT_SCALE);

static uint32_t rounded(float value)
{
    return (uint32_t)(value + 0.5f);
}

static void accumulate(const mb_measure_pass *pass, sad_pair errors, float block_mean)
{
    uint32_t *stats = words(pass->stats);
    if (pass->accumulate_error) {
        stats[MB_STAT_ERROR] += rounded(errors.plain * STAT_UNIT);
        stats[MB_STAT_ZERO_ERROR] += rounded(errors.zero_mean * STAT_UNIT);
    }
    int slot = pass->direction * 2;
    stats[MB_STAT_MEANS + slot] += rounded(block_mean);
    stats[MB_STAT_SQUARES + slot] += rounded(block_mean * block_mean * STAT_UNIT);
}

static void measure_block(const mb_measure_pass *pass, int id)
{
    vec4 *field = vec4s(pass->field);
    ivec2 origin = window_origin(block_coord(id, pass->grid));
    window x;
    window y;
    window_load_texels(&x, &pass->origin->texture, origin);
    window_load_exact(&y, &pass->target->texture, origin, v4_xy(field[id]));
    float weight = mean_weight(pass->mean_removal, fabsf(field[id].w));
    sad_result r = window_zsad(&x, &y, weight);
    float block_mean = window_sum(&x) * (255.0f / 256.0f);
    field[id] = (vec4){field[id].x, field[id].y, r.sad, block_dc(r.diff_sum)};
    accumulate(pass, window_sad_pair(&x, &y), block_mean);
}

void ref_measure(const mb_measure_pass *pass)
{
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        measure_block(pass, id);
    }
}
