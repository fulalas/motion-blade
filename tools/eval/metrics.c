#include "metrics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK = 16, SSIM_WINDOW = 8, SSIM_STEP = 4 };

static const double C1 = 6.5025;
static const double C2 = 58.5225;
static const double PERFECT_DB = 99.0;

static double decibels(double mse)
{
    return mse < 1e-10 ? PERFECT_DB : 10 * log10(255.0 * 255.0 / mse);
}

double metric_psnr(const uint8_t *a, const uint8_t *b, size_t count)
{
    double squared = 0;
    for (size_t i = 0; i < count; i++) {
        int d = a[i] - b[i];
        squared += d * d;
    }
    return decibels(squared / (double)count);
}

static int descending(const void *x, const void *y)
{
    double a = *(const double *)x;
    double b = *(const double *)y;
    return a < b ? 1 : a > b ? -1 : 0;
}

typedef struct plane_pair {
    const uint8_t *a;
    const uint8_t *b;
    int width;
} plane_pair;

static double block_mse(plane_pair p, int bx, int by)
{
    const uint8_t *a = p.a;
    const uint8_t *b = p.b;
    double squared = 0;
    for (int y = 0; y < BLOCK; y++) {
        for (int x = 0; x < BLOCK; x++) {
            size_t i = (size_t)(by * BLOCK + y) * (size_t)p.width + (size_t)(bx * BLOCK + x);
            int d = a[i] - b[i];
            squared += d * d;
        }
    }
    return squared / (BLOCK * BLOCK);
}

double metric_worst_blocks(const uint8_t *a, const uint8_t *b, int width, int height)
{
    int bw = width / BLOCK;
    int bh = height / BLOCK;
    int count = bw * bh;
    double *mse = malloc(sizeof(double) * (size_t)(count > 0 ? count : 1));
    if (!mse || count == 0) {
        free(mse);
        return PERFECT_DB;
    }
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            mse[by * bw + bx] = block_mse((plane_pair){a, b, width}, bx, by);
        }
    }
    qsort(mse, (size_t)count, sizeof(double), descending);
    int worst = count / 100 > 0 ? count / 100 : 1;
    double sum = 0;
    for (int i = 0; i < worst; i++) {
        sum += mse[i];
    }
    free(mse);
    return decibels(sum / worst);
}

typedef struct prefix_sums {
    int64_t *a;
    int64_t *b;
    int64_t *aa;
    int64_t *bb;
    int64_t *ab;
    size_t length;
} prefix_sums;

static void accumulate_rows(prefix_sums *p, const uint8_t *a, const uint8_t *b, int width)
{
    for (int x = 0; x < width; x++) {
        int va = a[x];
        int vb = b[x];
        p->a[x + 1] += va;
        p->b[x + 1] += vb;
        p->aa[x + 1] += va * va;
        p->bb[x + 1] += vb * vb;
        p->ab[x + 1] += va * vb;
    }
}

static void integrate(prefix_sums *p, int width)
{
    for (int x = 0; x < width; x++) {
        p->a[x + 1] += p->a[x];
        p->b[x + 1] += p->b[x];
        p->aa[x + 1] += p->aa[x];
        p->bb[x + 1] += p->bb[x];
        p->ab[x + 1] += p->ab[x];
    }
}

static double window_ssim(const prefix_sums *p, int x)
{
    double n = SSIM_WINDOW * SSIM_WINDOW;
    double sa = (double)(p->a[x + SSIM_WINDOW] - p->a[x]);
    double sb = (double)(p->b[x + SSIM_WINDOW] - p->b[x]);
    double saa = (double)(p->aa[x + SSIM_WINDOW] - p->aa[x]);
    double sbb = (double)(p->bb[x + SSIM_WINDOW] - p->bb[x]);
    double sab = (double)(p->ab[x + SSIM_WINDOW] - p->ab[x]);
    double ma = sa / n;
    double mb = sb / n;
    double va = saa / n - ma * ma;
    double vb = sbb / n - mb * mb;
    double cov = sab / n - ma * mb;
    return ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
}

double metric_ssim(const uint8_t *a, const uint8_t *b, int width, int height)
{
    size_t length = (size_t)width + 1;
    int64_t *storage = calloc(5 * length, sizeof(int64_t));
    if (!storage) {
        return 0;
    }
    prefix_sums p = {
        storage, storage + length, storage + 2 * length, storage + 3 * length, storage + 4 * length, length,
    };
    double sum = 0;
    int count = 0;
    for (int y = 0; y + SSIM_WINDOW <= height; y += SSIM_STEP) {
        memset(storage, 0, 5 * length * sizeof(int64_t));
        for (int j = 0; j < SSIM_WINDOW; j++) {
            accumulate_rows(&p, a + (size_t)(y + j) * (size_t)width, b + (size_t)(y + j) * (size_t)width, width);
        }
        integrate(&p, width);
        for (int x = 0; x + SSIM_WINDOW <= width; x += SSIM_STEP) {
            sum += window_ssim(&p, x);
            count++;
        }
    }
    free(storage);
    return count ? sum / count : 0;
}
