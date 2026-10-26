#ifndef MOTIONBLADE_H
#define MOTIONBLADE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_MAX_PENDING 7

typedef struct mb_ctx mb_ctx;

typedef struct mb_format {
    int w, h;
    int depth;
    int cshift_x, cshift_y;
    int nv;
    int lsb_pad;
} mb_format;

typedef struct mb_image {
    uint8_t *data[3];
    ptrdiff_t stride[3];
} mb_image;

typedef struct mb_stats {
    double interp_ms;
    double mean_err;
    int scene_cut;
} mb_stats;

typedef struct mb_dmabuf {
    int fd;
    int nplanes;
    uint32_t offset[3], pitch[3];
    uint64_t size;
    void *pool;
    int idx;
} mb_dmabuf;

typedef struct mb_dmabuf_in {
    int fd[4];
    uint64_t modifier;
    int obj[2];
    uint32_t offset[2], pitch[2];
    void (*release)(void *opaque);
    void *opaque;
} mb_dmabuf_in;

mb_ctx *mb_create(const mb_format *fmt);
void mb_destroy(mb_ctx *c);
void mb_reset(mb_ctx *c);
void mb_push(mb_ctx *c, const mb_image *src);
void mb_flush(mb_ctx *c);
int mb_interpolate(mb_ctx *c, double t, mb_image *dst);
int mb_submit(mb_ctx *c, double t);
int mb_retrieve(mb_ctx *c, mb_image *dst);
int mb_pending(mb_ctx *c);
int mb_ready(mb_ctx *c);
int mb_debug_field(mb_ctx *c, int which, float *out, int *w, int *h);
void mb_get_stats(mb_ctx *c, mb_stats *st);
int mb_set_param(mb_ctx *c, const char *name, double value);
int mb_export_init(mb_ctx *c);
int mb_retrieve_dmabuf(mb_ctx *c, mb_dmabuf *out);
int mb_export_image(mb_ctx *c, const mb_image *src, mb_dmabuf *out);
void mb_dmabuf_release(void *pool, int idx);
int mb_push_dmabuf(mb_ctx *c, const mb_dmabuf_in *in);

#ifdef __cplusplus
}
#endif

#endif
