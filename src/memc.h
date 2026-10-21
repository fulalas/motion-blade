#ifndef MEMC_H
#define MEMC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMC_MAX_PENDING 7

typedef struct memc_ctx memc_ctx;

typedef struct memc_format {
    int w, h;
    int depth;
    int cshift_x, cshift_y;
    int nv;
} memc_format;

typedef struct memc_image {
    uint8_t *data[3];
    ptrdiff_t stride[3];
} memc_image;

typedef struct memc_stats {
    double me_ms, interp_ms;
    double mean_err;
    int scene_cut;
} memc_stats;

memc_ctx *memc_create(const memc_format *fmt, int threads);
void memc_destroy(memc_ctx *c);
void memc_reset(memc_ctx *c);
void memc_push(memc_ctx *c, const memc_image *src);
int memc_interpolate(memc_ctx *c, double t, memc_image *dst);
int memc_submit(memc_ctx *c, double t);
int memc_retrieve(memc_ctx *c, memc_image *dst);
int memc_pending(memc_ctx *c);
int memc_ready(memc_ctx *c);
int memc_debug_field(memc_ctx *c, int which, float *out, int *w, int *h);
void memc_get_stats(memc_ctx *c, memc_stats *st);
int memc_set_param(memc_ctx *c, const char *name, double value);

#ifdef __cplusplus
}
#endif

#endif
