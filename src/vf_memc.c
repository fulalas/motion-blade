#include <math.h>

#include "common/common.h"
#include "common/msg.h"
#include "filters/f_autoconvert.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"

#include "video/filter/memc/memc.h"

#define MAXQ 32

struct opts {
    int factor;
    double max_fps;
    double max_gap;
};

struct qent {
    struct mp_frame frame;
    bool gpu;
};

struct priv {
    struct opts *opts;
    struct mp_vo_opts *vo_opts;
    struct mp_pin *in;
    struct mp_image_pool *pool;
    memc_ctx *ctx;
    int imgfmt, w, h;
    bool disabled;
    struct mp_image *prev;
    struct qent outq[MAXQ];
    int nout, ngpu;
    double grid, dfps;
    int64_t dfps_time;
    int expect;
};

static void clear_state(struct priv *p)
{
    mp_image_unrefp(&p->prev);
    p->grid = MP_NOPTS_VALUE;
    if (p->ctx)
        memc_reset(p->ctx);
    for (int i = 0; i < p->nout; i++)
        mp_frame_unref(&p->outq[i].frame);
    p->nout = 0;
    p->ngpu = 0;
}

static void f_reset(struct mp_filter *f)
{
    clear_state(f->priv);
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    clear_state(p);
    memc_destroy(p->ctx);
    p->ctx = NULL;
}

static memc_image to_memc(struct mp_image *mpi)
{
    memc_image im = {0};
    for (int i = 0; i < 3 && i < mpi->num_planes; i++) {
        im.data[i] = mpi->planes[i];
        im.stride[i] = mpi->stride[i];
    }
    return im;
}

static bool setup(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    if (p->imgfmt == mpi->imgfmt && p->w == mpi->w && p->h == mpi->h)
        return !p->disabled;
    clear_state(p);
    memc_destroy(p->ctx);
    p->ctx = NULL;
    p->disabled = true;
    p->imgfmt = mpi->imgfmt;
    p->w = mpi->w;
    p->h = mpi->h;

    struct mp_regular_imgfmt reg;
    if (!mp_get_regular_imgfmt(&reg, mpi->imgfmt) || reg.component_type != MP_COMPONENT_TYPE_UINT ||
        reg.component_size > 2 || reg.chroma_xs > 1 || reg.chroma_ys > 1) {
        MP_WARN(f, "unsupported format %s, passing through\n", mp_imgfmt_to_name(mpi->imgfmt));
        return false;
    }
    memc_format fmt = {
        .w = mpi->w,
        .h = mpi->h,
        .depth = reg.component_size == 1 ? 8 : 16 - (reg.component_pad < 0 ? -reg.component_pad : 0),
        .cshift_x = reg.chroma_xs,
        .cshift_y = reg.chroma_ys,
    };
    if (reg.num_planes == 2 && reg.planes[0].num_components == 1 && reg.planes[1].num_components == 2) {
        fmt.nv = 1;
    } else if (!(reg.num_planes == 3 && reg.planes[0].num_components == 1 && reg.planes[1].num_components == 1 &&
                 reg.planes[2].num_components == 1)) {
        MP_WARN(f, "unsupported plane layout %s, passing through\n", mp_imgfmt_to_name(mpi->imgfmt));
        return false;
    }
    p->ctx = memc_create(&fmt, 0);
    if (!p->ctx) {
        MP_ERR(f, "could not initialize Vulkan interpolator, passing through\n");
        return false;
    }
    p->disabled = false;
    MP_VERBOSE(f, "interpolating %dx%d %s\n", mpi->w, mpi->h, mp_imgfmt_to_name(mpi->imgfmt));
    return true;
}

static void enqueue(struct priv *p, struct mp_frame frame, bool gpu)
{
    if (p->nout == MAXQ) {
        mp_frame_unref(&frame);
        return;
    }
    p->outq[p->nout++] = (struct qent){frame, gpu};
    if (gpu)
        p->ngpu++;
}

static void finish_oldest_gpu(struct priv *p)
{
    for (int i = 0; i < p->nout; i++) {
        struct qent *e = &p->outq[i];
        if (!e->gpu)
            continue;
        memc_image dst = to_memc(e->frame.data);
        memc_retrieve(p->ctx, &dst);
        e->gpu = false;
        p->ngpu--;
        return;
    }
}

static double display_fps(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (p->vo_opts && p->vo_opts->display_fps_override > 0)
        return p->vo_opts->display_fps_override;
    int64_t now = mp_time_ns();
    if (!p->dfps_time || now - p->dfps_time > MP_TIME_S_TO_NS(10)) {
        struct mp_stream_info *info = mp_filter_find_stream_info(f);
        p->dfps = info && info->get_display_fps ? info->get_display_fps(info) : 0;
        p->dfps_time = now;
    }
    return p->dfps;
}

static void enqueue_ref(struct priv *p, struct mp_image *img, double pts, double dur)
{
    struct mp_image *r = mp_image_new_ref(img);
    if (!r)
        return;
    r->pts = pts;
    r->pkt_duration = dur;
    enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, r), false);
}

static void submit_interp(struct priv *p, struct mp_image *ref, double t, double pts, double dur)
{
    struct mp_image *out = mp_image_pool_get(p->pool, ref->imgfmt, ref->w, ref->h);
    if (out) {
        mp_image_copy_attributes(out, ref);
        while (memc_pending(p->ctx) >= MEMC_MAX_PENDING)
            finish_oldest_gpu(p);
        if (memc_submit(p->ctx, t) == 0) {
            out->pts = pts;
            out->pkt_duration = dur;
            enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, out), true);
            return;
        }
        talloc_free(out);
    }
    enqueue_ref(p, ref, pts, dur);
}

static void handle_frame(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    if (!setup(f, mpi)) {
        mp_image_unrefp(&p->prev);
        enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, mpi), false);
        return;
    }
    memc_image src = to_memc(mpi);
    if (!p->prev)
        memc_reset(p->ctx);
    memc_push(p->ctx, &src);

    struct mp_image *prev = p->prev;
    double gap = prev ? mpi->pts - prev->pts : 0;
    bool ok = prev && prev->pts != MP_NOPTS_VALUE && mpi->pts != MP_NOPTS_VALUE && gap > 0 &&
              gap <= p->opts->max_gap && (p->opts->max_fps <= 0 || 1.0 / gap <= p->opts->max_fps);
    double dfps = p->opts->factor ? 0 : display_fps(f);
    int n = p->opts->factor ? p->opts->factor : 2;

    if (dfps > 0) {
        double step = 1.0 / dfps;
        if (!ok || gap < step * 1.05) {
            enqueue_ref(p, mpi, mpi->pts, gap > 0 ? gap : step);
            p->grid = mpi->pts + step;
        } else {
            if (p->grid == MP_NOPTS_VALUE || p->grid <= prev->pts || p->grid > mpi->pts + step)
                p->grid = prev->pts + step;
            int count = 0;
            for (; p->grid < mpi->pts + step * 0.01; p->grid += step) {
                double t = (p->grid - prev->pts) / gap;
                if (t > 0.99) {
                    enqueue_ref(p, mpi, p->grid, step);
                } else {
                    submit_interp(p, t < 0.5 ? prev : mpi, t, p->grid, step);
                    count++;
                }
            }
            p->expect = MPMAX(1, count);
        }
        mp_image_unrefp(&p->prev);
        p->prev = mpi;
        return;
    }

    if (ok) {
        for (int k = 1; k < n; k++) {
            double t = (double)k / n;
            submit_interp(p, t < 0.5 ? prev : mpi, t, prev->pts + gap * t, gap / n);
        }
        p->expect = n - 1;
        mpi->pkt_duration = gap / n;
    }
    mp_image_unrefp(&p->prev);
    p->prev = mp_image_new_ref(mpi);
    enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, mpi), false);
}

static bool emit_head(struct mp_filter *f, bool block)
{
    struct priv *p = f->priv;
    if (!p->nout)
        return false;
    struct qent *h = &p->outq[0];
    if (h->gpu) {
        if (!block && !memc_ready(p->ctx))
            return false;
        finish_oldest_gpu(p);
    }
    struct mp_frame frame = h->frame;
    p->nout--;
    memmove(&p->outq[0], &p->outq[1], p->nout * sizeof(p->outq[0]));
    mp_pin_in_write(f->ppins[1], frame);
    return true;
}

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    bool want_out = mp_pin_in_needs_data(f->ppins[1]);

    if (want_out && emit_head(f, false)) {
        mp_filter_internal_mark_progress(f);
        return;
    }

    bool eof_queued = p->nout && p->outq[p->nout - 1].frame.type == MP_FRAME_EOF;
    if (!eof_queued && (p->ngpu == 0 || p->ngpu + p->expect <= MEMC_MAX_PENDING) && p->nout < MAXQ - 8 &&
        mp_pin_out_request_data(p->in)) {
        struct mp_frame frame = mp_pin_out_read(p->in);
        if (frame.type == MP_FRAME_VIDEO) {
            handle_frame(f, frame.data);
        } else if (frame.type != MP_FRAME_NONE) {
            if (frame.type == MP_FRAME_EOF)
                mp_image_unrefp(&p->prev);
            enqueue(p, frame, false);
        }
        mp_filter_internal_mark_progress(f);
        return;
    }

    if (want_out && emit_head(f, true))
        mp_filter_internal_mark_progress(f);
}

static const struct mp_filter_info filter = {
    .name = "memc",
    .process = f_process,
    .reset = f_reset,
    .destroy = f_destroy,
    .priv_size = sizeof(struct priv),
};

static struct mp_filter *f_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    p->pool = mp_image_pool_new(p);
    p->vo_opts = mp_get_config_group(p, f->global, &vo_sub_opts);
    p->grid = MP_NOPTS_VALUE;
    p->expect = 1;

    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv) {
        talloc_free(f);
        return NULL;
    }
    static const char *const fmts[] = {"yuv420p", "nv12", "p010", "yuv420p10", "yuv422p", "yuv444p",
                                       "yuv422p10", "yuv444p10", "p016", "yuv420p16"};
    for (int i = 0; i < MP_ARRAY_SIZE(fmts); i++) {
        int fmt = mp_imgfmt_from_name(bstr0(fmts[i]));
        if (fmt)
            mp_autoconvert_add_imgfmt(conv, fmt, 0);
    }
    mp_pin_connect(conv->f->pins[0], f->ppins[0]);
    p->in = conv->f->pins[1];
    return f;
}

#define OPT_BASE_STRUCT struct opts
static const m_option_t opts_list[] = {
    {"factor", OPT_CHOICE(factor, {"auto", 0}), M_RANGE(2, 8)},
    {"max-fps", OPT_DOUBLE(max_fps), M_RANGE(0, 1000)},
    {"max-gap", OPT_DOUBLE(max_gap), M_RANGE(0, 10)},
    {0}
};

const struct mp_user_filter_entry vf_memc = {
    .desc = {
        .description = "motion-compensated frame interpolation",
        .name = "memc",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const struct opts){
            .factor = 0,
            .max_fps = 0,
            .max_gap = 0.25,
        },
        .options = opts_list,
    },
    .create = f_create,
};
