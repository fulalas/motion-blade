#include <math.h>

#include "common/vec.h"
#include "motionblade/layout.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

typedef struct series {
    int count_slot;
    int values_slot;
} series;

static const series ERRORS = {MB_HIST_ERROR_COUNT, MB_HIST_ERRORS};
static const series ZEROS = {MB_HIST_ZERO_COUNT, MB_HIST_ZEROS};

static float series_median(const float *history, series s)
{
    int n = (int)history[s.count_slot];
    float v[MB_HISTORY_LENGTH];
    for (int i = 0; i < n; i++) {
        v[i] = history[s.values_slot + i];
    }
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            float swap = v[j];
            v[j] = v[j - 1];
            v[j - 1] = swap;
        }
    }
    return v[n / 2];
}

static void series_push(float *history, series s, float value)
{
    int n = (int)history[s.count_slot];
    int next = (int)history[s.count_slot + 1];
    history[s.values_slot + next] = value;
    history[s.count_slot + 1] = (float)((next + 1) % MB_HISTORY_LENGTH);
    history[s.count_slot] = (float)(n + 1 < MB_HISTORY_LENGTH ? n + 1 : MB_HISTORY_LENGTH);
}

static float brightness_spread(const uint32_t *stats, int direction, float blocks)
{
    int slot = direction * 2;
    float mean = (float)stats[MB_STAT_MEANS + slot] / blocks;
    return sqrtf(fmaxf((float)stats[MB_STAT_SQUARES + slot] * (float)MB_STAT_SCALE / blocks - mean * mean, 0.0f));
}

static float contrast_factor(const mb_cut_pass *pass, const uint32_t *stats)
{
    float blocks = (float)pass->block_count;
    float spread = fmaxf(brightness_spread(stats, 0, blocks), brightness_spread(stats, 1, blocks));
    return pass->spread_norm > 0.0f ? clampf(spread / pass->spread_norm, pass->spread_min, 1.0f) : 1.0f;
}

static float error_threshold(const mb_cut_pass *pass, const float *history, float k)
{
    if (history[ERRORS.count_slot] <= 0.0f) {
        return pass->max_threshold * k;
    }
    float relative = pass->error_ratio * series_median(history, ERRORS) + MB_CUT_ERROR_OFFSET * k;
    return fminf(pass->max_threshold * k, fmaxf(MB_CUT_ERROR_FLOOR * k, relative));
}

static float zero_threshold(const mb_cut_pass *pass, const float *history, float k)
{
    if (history[ZEROS.count_slot] <= 0.0f) {
        return 0.0f;
    }
    return fmaxf(pass->zero_mean_ratio * series_median(history, ZEROS), pass->zero_mean_min * k);
}

void ref_decide_cut(const mb_cut_pass *pass)
{
    uint32_t *stats = words(pass->stats);
    float *history = floats(pass->history);
    float error = (float)stats[MB_STAT_ERROR] * pass->error_scale;
    float zero_mean = (float)stats[MB_STAT_ZERO_ERROR] * pass->error_scale;
    float k = contrast_factor(pass, stats);
    bool error_jump = error > error_threshold(pass, history, k);
    bool cut = error_jump && zero_mean > zero_threshold(pass, history, k);
    if (error > MB_CUT_MIN_RECORDED_ERROR && !error_jump) {
        series_push(history, ERRORS, error);
    }
    if (error > MB_CUT_MIN_RECORDED_ERROR && !cut) {
        series_push(history, ZEROS, zero_mean);
    }
    stats[MB_STAT_CUT] = cut ? 1u : 0u;
    history[MB_HIST_LAST_CUT] = cut ? 1.0f : 0.0f;
}
