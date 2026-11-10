#include "estimation/window.h"

#include <math.h>

enum { N = MB_WINDOW_SIZE };

static const float LEVELS = 255.0f;
static const float PIXELS = (float)(MB_WINDOW_SIZE * MB_WINDOW_SIZE);

void window_load_texels(window *w, const ref_texture *texture, ivec2 origin)
{
    for (int lane = 0; lane < N; lane++) {
        for (int x = 0; x < N; x++) {
            w->rows[lane][x] = ref_texel(texture, origin.x + x, origin.y + lane, 0);
        }
    }
}

static vec2 lane_origin(ivec2 origin, int lane)
{
    return v2((float)origin.x + 0.5f, (float)(origin.y + lane) + 0.5f);
}

static void load_exact_row(float row[N], const ref_texture *texture, vec2 position)
{
    vec2 q = v2_sub(position, v2(0.5f, 0.5f));
    float fx = floorf(q.x);
    float fy = floorf(q.y);
    float wx = q.x - fx;
    float wy = q.y - fy;
    for (int x = 0; x < N; x++) {
        int tx = (int)fx + x;
        float top = mixf(ref_texel(texture, tx, (int)fy, 0), ref_texel(texture, tx + 1, (int)fy, 0), wx);
        float bottom = mixf(ref_texel(texture, tx, (int)fy + 1, 0), ref_texel(texture, tx + 1, (int)fy + 1, 0), wx);
        row[x] = mixf(top, bottom, wy);
    }
}

void window_load_exact(window *w, const ref_texture *texture, ivec2 origin, vec2 offset)
{
    for (int lane = 0; lane < N; lane++) {
        load_exact_row(w->rows[lane], texture, v2_add(lane_origin(origin, lane), offset));
    }
}

void window_load_hardware_sparse(window *w, const ref_texture *texture, ivec2 origin, vec2 offset, int stride)
{
    vec2 inv = v2_inverse_size(texture->width, texture->height);
    for (int lane = 0; lane < N; lane++) {
        vec2 p = v2_add(lane_origin(origin, lane), offset);
        for (int x = lane & (stride - 1); x < N; x += stride) {
            w->rows[lane][x] = ref_sample(texture, (p.x + (float)x) * inv.x, p.y * inv.y, 0);
        }
    }
}

void window_load_hardware(window *w, const ref_texture *texture, ivec2 origin, vec2 offset)
{
    window_load_hardware_sparse(w, texture, origin, offset, 1);
}

float cluster_sum(const float lanes[N])
{
    float level[N];
    for (int i = 0; i < N; i++) {
        level[i] = lanes[i];
    }
    for (int width = N / 2; width > 0; width /= 2) {
        for (int i = 0; i < width; i++) {
            level[i] = level[2 * i] + level[2 * i + 1];
        }
    }
    return level[0];
}

static float row_diff_sum(const float a[N], const float b[N])
{
    float sum = 0.0f;
    for (int x = 0; x < N; x++) {
        sum += a[x] - b[x];
    }
    return sum;
}

static float row_abs_sum(const float a[N], const float b[N], float mean)
{
    float sum = 0.0f;
    for (int x = 0; x < N; x++) {
        sum += fabsf(a[x] - b[x] - mean);
    }
    return sum;
}

static float total_diff(const window *a, const window *b)
{
    float lanes[N];
    for (int lane = 0; lane < N; lane++) {
        lanes[lane] = row_diff_sum(a->rows[lane], b->rows[lane]);
    }
    return cluster_sum(lanes);
}

static float total_abs(const window *a, const window *b, float mean)
{
    float lanes[N];
    for (int lane = 0; lane < N; lane++) {
        lanes[lane] = row_abs_sum(a->rows[lane], b->rows[lane], mean);
    }
    return cluster_sum(lanes) * LEVELS;
}

sad_result window_zsad(const window *a, const window *b, float mean_weight)
{
    return window_zsad_sparse(a, b, mean_weight, 1);
}

static float sparse_diff_sum(const float a[N], const float b[N], int stride, int phase)
{
    float sum = 0.0f;
    for (int x = phase; x < N; x += stride) {
        sum += a[x] - b[x];
    }
    return sum;
}

static float sparse_abs_sum(const float a[N], const float b[N], int stride, int phase, float mean)
{
    float sum = 0.0f;
    for (int x = phase; x < N; x += stride) {
        sum += fabsf(a[x] - b[x] - mean);
    }
    return sum;
}

sad_result window_zsad_sparse(const window *a, const window *b, float mean_weight, int stride)
{
    float diffs[N], sums[N];
    for (int lane = 0; lane < N; lane++) {
        diffs[lane] = sparse_diff_sum(a->rows[lane], b->rows[lane], stride, lane & (stride - 1));
    }
    float diff_sum = cluster_sum(diffs);
    float mean = mean_weight > 0.0f ? diff_sum / (PIXELS / (float)stride) * mean_weight : 0.0f;
    for (int lane = 0; lane < N; lane++) {
        sums[lane] = sparse_abs_sum(a->rows[lane], b->rows[lane], stride, lane & (stride - 1), mean);
    }
    return (sad_result){cluster_sum(sums) * LEVELS * (float)stride, diff_sum * (float)stride};
}

sad_pair window_sad_pair(const window *a, const window *b)
{
    float mean = total_diff(a, b) / PIXELS;
    return (sad_pair){total_abs(a, b, 0.0f), total_abs(a, b, mean)};
}

float window_sum(const window *w)
{
    float lanes[N];
    for (int lane = 0; lane < N; lane++) {
        float sum = 0.0f;
        for (int x = 0; x < N; x++) {
            sum += w->rows[lane][x];
        }
        lanes[lane] = sum;
    }
    return cluster_sum(lanes);
}

float window_texture(const window *w, const window *below)
{
    float lanes[N];
    for (int lane = 0; lane < N; lane++) {
        float sum = 0.0f;
        for (int x = 0; x < N - 1; x++) {
            sum += fabsf(w->rows[lane][x + 1] - w->rows[lane][x]) + fabsf(below->rows[lane][x] - w->rows[lane][x]);
        }
        lanes[lane] = sum;
    }
    return cluster_sum(lanes) * LEVELS;
}

float block_dc(float diff_sum)
{
    return diff_sum * (LEVELS / PIXELS);
}

float mean_weight(mb_mean_removal removal, float min_abs_dc)
{
    return smoothstepf(removal.lo, removal.hi, min_abs_dc);
}
