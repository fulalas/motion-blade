#ifndef EVAL_METRICS_H
#define EVAL_METRICS_H

#include <stddef.h>
#include <stdint.h>

double metric_psnr(const uint8_t *a, const uint8_t *b, size_t count);
double metric_worst_blocks(const uint8_t *a, const uint8_t *b, int width, int height);
double metric_ssim(const uint8_t *a, const uint8_t *b, int width, int height);

#endif
