#ifndef TOOLS_SAMPLES_H
#define TOOLS_SAMPLES_H

#include <math.h>
#include <stdint.h>

static inline uint8_t clamp_byte(double v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : lrint(v));
}

static inline double psnr_decibels(double mse)
{
    return mse < 1e-10 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

#endif
