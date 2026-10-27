#include <math.h>
#include <unistd.h>

#include "config.h"

#if HAVE_DRM
#include <drm_fourcc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#endif

#if HAVE_VULKAN
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#endif

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
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"

#include "video/filter/motionblade/motionblade.h"

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
    struct m_config_cache *vo_opts_cache;
    struct mp_pin *in;
    struct mp_image_pool *pool;
    mb_ctx *ctx;
    int imgfmt, w, h;
    bool disabled;
    struct mp_image *prev, *cur;
    struct qent *outq;
    int nout, ngpu;
    double grid, dfps, last_pts;
    bool fast;
    int64_t dfps_time;
    int expect;
    struct AVBufferRef *xframes;
    uint32_t fourcc;
    bool hw_failed, nozc;
    struct AVBufferRef *vkdev, *vkframes;
};

static void clear_state(struct priv *p)
{
    mp_image_unrefp(&p->prev);
    mp_image_unrefp(&p->cur);
    p->grid = MP_NOPTS_VALUE;
    p->last_pts = MP_NOPTS_VALUE;
    p->fast = false;
    p->hw_failed = false;
    if (p->ctx)
        mb_reset(p->ctx);
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
    mb_destroy(p->ctx);
    p->ctx = NULL;
    av_buffer_unref(&p->xframes);
    av_buffer_unref(&p->vkframes);
    av_buffer_unref(&p->vkdev);
}

static mb_image to_mb(struct mp_image *mpi)
{
    mb_image im = {0};
    for (int i = 0; i < 3 && i < mpi->num_planes; i++) {
        im.data[i] = mpi->planes[i];
        im.stride[i] = mpi->stride[i];
    }
    return im;
}

static void finish_oldest_gpu(struct priv *p);
static void flush_pending(struct mp_filter *f);

static void setup_export(struct mp_filter *f, struct mp_image *mpi, int swfmt)
{
#if HAVE_DRM
    struct priv *p = f->priv;
    p->fourcc = swfmt == IMGFMT_NV12   ? DRM_FORMAT_NV12
                : swfmt == IMGFMT_P010 ? DRM_FORMAT_P010
                : swfmt == IMGFMT_420P ? DRM_FORMAT_YUV420
                                       : 0;
    if (!p->fourcc || p->nozc)
        return;
    struct mp_hwdec_ctx *hw = mp_filter_load_hwdec_device(f, IMGFMT_DRMPRIME, AV_HWDEVICE_TYPE_DRM);
    if (hw && mp_update_av_hw_frames_pool(&p->xframes, hw->av_device_ref, IMGFMT_DRMPRIME, swfmt, mpi->w, mpi->h, false) &&
        mb_export_init(p->ctx) < 0)
        av_buffer_unref(&p->xframes);
#else
    (void)f;
    (void)mpi;
    (void)swfmt;
#endif
}

#if HAVE_DRM
struct xrel {
    void *pool;
    int idx;
};

static void drm_free(void *opaque, uint8_t *data)
{
    struct xrel *r = opaque;
    AVDRMFrameDescriptor *d = (AVDRMFrameDescriptor *)data;
    close(d->objects[0].fd);
    mb_dmabuf_release(r->pool, r->idx);
    av_free(r);
    av_free(d);
}
#endif

static struct mp_image *drm_image(struct priv *p, const mb_dmabuf *x, struct mp_image *ref)
{
#if HAVE_DRM
    AVDRMFrameDescriptor *d = av_mallocz(sizeof(*d));
    struct xrel *r = av_mallocz(sizeof(*r));
    AVFrame *fr = av_frame_alloc();
    if (!d || !r || !fr)
        goto fail;
    d->nb_objects = 1;
    d->objects[0].fd = x->fd;
    d->objects[0].size = x->size;
    d->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
    d->nb_layers = 1;
    d->layers[0].format = p->fourcc;
    d->layers[0].nb_planes = x->nplanes;
    for (int i = 0; i < x->nplanes; i++) {
        d->layers[0].planes[i].object_index = 0;
        d->layers[0].planes[i].offset = x->offset[i];
        d->layers[0].planes[i].pitch = x->pitch[i];
    }
    r->pool = x->pool;
    r->idx = x->idx;
    fr->format = AV_PIX_FMT_DRM_PRIME;
    fr->width = ref->w;
    fr->height = ref->h;
    fr->data[0] = (uint8_t *)d;
    fr->buf[0] = av_buffer_create((uint8_t *)d, sizeof(*d), drm_free, r, 0);
    if (!fr->buf[0])
        goto fail;
    fr->hw_frames_ctx = av_buffer_ref(p->xframes);
    struct mp_image *img = mp_image_from_av_frame(fr);
    av_frame_free(&fr);
    if (img)
        mp_image_copy_attributes(img, ref);
    return img;
fail:
    av_frame_free(&fr);
    av_free(r);
    av_free(d);
    close(x->fd);
    mb_dmabuf_release(x->pool, x->idx);
#else
    (void)p;
    (void)x;
    (void)ref;
#endif
    return NULL;
}

static void free_img(void *opaque)
{
    talloc_free(opaque);
}

static struct mp_image *map_drm(struct priv *p, struct mp_image *mpi)
{
    struct mp_image *img = NULL;
#if HAVE_DRM
    if (mpi->imgfmt != IMGFMT_VAAPI || p->hw_failed || !p->xframes)
        return NULL;
    AVFrame *av = mp_image_to_av_frame(mpi), *d = av_frame_alloc();
    if (av && d) {
        d->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(d, av, AV_HWFRAME_MAP_READ) >= 0) {
            if (!d->hw_frames_ctx)
                d->hw_frames_ctx = av_buffer_ref(p->xframes);
            img = mp_image_from_av_frame(d);
        }
    }
    av_frame_free(&av);
    av_frame_free(&d);
    if (img)
        mp_image_copy_attributes(img, mpi);
#else
    (void)p;
    (void)mpi;
#endif
    return img;
}

static bool push_hw(struct priv *p, struct mp_image *mpi)
{
    struct mp_image *m = map_drm(p, mpi);
    mb_dmabuf_in in = {.release = free_img, .opaque = m};
    int n = 0;
#if HAVE_DRM
    const AVDRMFrameDescriptor *x = m ? (AVDRMFrameDescriptor *)m->planes[0] : NULL;
    for (int i = 0; x && i < x->nb_layers; i++)
        for (int j = 0; j < x->layers[i].nb_planes && n < 2; j++, n++) {
            in.obj[n] = x->layers[i].planes[j].object_index;
            in.offset[n] = x->layers[i].planes[j].offset;
            in.pitch[n] = x->layers[i].planes[j].pitch;
        }
    for (int i = 0; x && i < x->nb_objects && i < 4; i++)
        in.fd[i] = x->objects[i].fd;
    in.modifier = x ? x->objects[0].format_modifier : 0;
#endif
    if (n != 2 || mb_push_dmabuf(p->ctx, &in) < 0) {
        talloc_free(m);
        p->hw_failed = true;
        return false;
    }
    return true;
}

#if HAVE_VULKAN
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static void vk_lock(void *opaque, uint32_t qf, uint32_t qi)
{
    AVHWDeviceContext *dc = opaque;
    AVVulkanDeviceContext *vk = dc->hwctx;
#if FF_API_VULKAN_SYNC_QUEUES
    if (vk->lock_queue)
        vk->lock_queue(dc, qf, qi);
#else
    (void)vk, (void)qf, (void)qi;
#endif
}

static void vk_unlock(void *opaque, uint32_t qf, uint32_t qi)
{
    AVHWDeviceContext *dc = opaque;
    AVVulkanDeviceContext *vk = dc->hwctx;
#if FF_API_VULKAN_SYNC_QUEUES
    if (vk->unlock_queue)
        vk->unlock_queue(dc, qf, qi);
#else
    (void)vk, (void)qf, (void)qi;
#endif
}
#pragma GCC diagnostic pop

static mb_ctx *create_vk(struct mp_filter *f, struct mp_image *mpi, const mb_format *fmt)
{
    struct priv *p = f->priv;
    AVHWFramesContext *hwfc = (AVHWFramesContext *)mpi->hwctx->data;
    AVHWDeviceContext *dc = hwfc->device_ctx;
    AVVulkanDeviceContext *vk = dc->hwctx;
    int qf = -1;
    for (int i = 0; i < vk->nb_qf && qf < 0; i++)
        if (vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT)
            qf = vk->qf[i].idx;
    for (int i = 0; i < vk->nb_qf && qf < 0; i++)
        if (vk->qf[i].flags & VK_QUEUE_GRAPHICS_BIT)
            qf = vk->qf[i].idx;
    if (qf < 0 || !fmt->nv)
        return NULL;
    mb_vkdev d = {
        .inst = vk->inst,
        .pd = vk->phys_dev,
        .dev = vk->act_dev,
        .qf = qf,
        .features = &vk->device_features,
        .lock = vk_lock,
        .unlock = vk_unlock,
        .opaque = dc,
    };
    mb_ctx *c = mb_create_vk(fmt, &d);
    if (!c)
        return NULL;
    p->vkdev = av_buffer_ref(hwfc->device_ref);
    if (!p->vkdev || !mp_update_av_hw_frames_pool(&p->vkframes, hwfc->device_ref, IMGFMT_VULKAN, mpi->params.hw_subfmt,
                                                  mpi->w, mpi->h, false)) {
        mb_destroy(c);
        av_buffer_unref(&p->vkdev);
        return NULL;
    }
    return c;
}

static bool vk_frame(struct mp_image *mpi, mb_vkframe *vf, AVHWFramesContext **fc, AVVkFrame **vkf)
{
    *fc = (AVHWFramesContext *)mpi->hwctx->data;
    *vkf = (AVVkFrame *)mpi->planes[0];
    AVVulkanFramesContext *vkfc = (*fc)->hwctx;
    vkfc->lock_frame(*fc, *vkf);
    *vf = (mb_vkframe){0};
    while (vf->nimg < 2 && (*vkf)->img[vf->nimg]) {
        int i = vf->nimg++;
        vf->img[i] = (*vkf)->img[i];
        vf->layout[i] = (*vkf)->layout[i];
        vf->sem[i] = (*vkf)->sem[i];
        vf->sem_value[i] = (*vkf)->sem_value[i];
        vf->qfam[i] = (*vkf)->queue_family[i];
    }
    return vf->nimg > 0;
}

static void vk_frame_done(const mb_vkframe *vf, AVHWFramesContext *fc, AVVkFrame *vkf, bool used, VkAccessFlags access)
{
    for (int i = 0; used && i < vf->nimg; i++) {
        vkf->layout[i] = vf->layout[i];
        vkf->sem_value[i] = vf->sem_value[i];
        vkf->access[i] = access;
    }
    AVVulkanFramesContext *vkfc = fc->hwctx;
    vkfc->unlock_frame(fc, vkf);
}

static bool push_vk(struct priv *p, struct mp_image *mpi)
{
    mb_vkframe vf;
    AVHWFramesContext *fc;
    AVVkFrame *vkf;
    bool ok = vk_frame(mpi, &vf, &fc, &vkf) && mb_push_vk(p->ctx, &vf) == 0;
    vk_frame_done(&vf, fc, vkf, ok, VK_ACCESS_TRANSFER_READ_BIT);
    return ok;
}

static struct mp_image *submit_vk(struct priv *p, struct mp_image *ref, double t)
{
    AVFrame *fr = av_frame_alloc();
    struct mp_image *out = NULL;
    if (fr && av_hwframe_get_buffer(p->vkframes, fr, 0) >= 0)
        out = mp_image_from_av_frame(fr);
    av_frame_free(&fr);
    if (!out)
        return NULL;
    mb_vkframe vf;
    AVHWFramesContext *fc;
    AVVkFrame *vkf;
    bool ok = vk_frame(out, &vf, &fc, &vkf) && mb_submit_vk(p->ctx, t, &vf) == 0;
    vk_frame_done(&vf, fc, vkf, ok, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!ok) {
        talloc_free(out);
        return NULL;
    }
    mp_image_copy_attributes(out, ref);
    return out;
}
#endif

static bool is_hw(struct mp_image *mpi)
{
    return mpi->imgfmt == IMGFMT_VAAPI || mpi->imgfmt == IMGFMT_VULKAN;
}

static struct mp_image *download(struct priv *p, struct mp_image *mpi)
{
    if (!is_hw(mpi))
        return mpi;
    struct mp_image *sw = mp_image_hw_download(mpi, p->pool);
    talloc_free(mpi);
    return sw;
}

static bool push_gpu(struct priv *p, struct mp_image *mpi)
{
    if (mpi->imgfmt == IMGFMT_VAAPI)
        return push_hw(p, mpi);
#if HAVE_VULKAN
    if (p->vkframes && push_vk(p, mpi))
        return true;
#endif
    p->hw_failed = true;
    return false;
}

static struct mp_image *to_drm(struct priv *p, struct mp_image *mpi)
{
    mb_dmabuf x;
    mb_image src = to_mb(mpi);
    if (!p->xframes || mpi->imgfmt != p->imgfmt || mb_export_image(p->ctx, &src, &x) < 0)
        return mpi;
    struct mp_image *img = drm_image(p, &x, mpi);
    if (!img)
        return mpi;
    talloc_free(mpi);
    return img;
}

static bool setup(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    int swfmt = is_hw(mpi) ? mpi->params.hw_subfmt : mpi->imgfmt;
    if (p->imgfmt == swfmt && p->w == mpi->w && p->h == mpi->h)
        return !p->disabled;
    flush_pending(f);
    while (p->ngpu)
        finish_oldest_gpu(p);
    mp_image_unrefp(&p->prev);
    p->grid = MP_NOPTS_VALUE;
    mb_destroy(p->ctx);
    p->ctx = NULL;
    av_buffer_unref(&p->xframes);
    av_buffer_unref(&p->vkframes);
    av_buffer_unref(&p->vkdev);
    p->disabled = true;
    p->imgfmt = swfmt;
    p->w = mpi->w;
    p->h = mpi->h;

    struct mp_regular_imgfmt reg;
    if (!mp_get_regular_imgfmt(&reg, swfmt) || reg.component_type != MP_COMPONENT_TYPE_UINT ||
        reg.component_size > 2 || reg.chroma_xs > 1 || reg.chroma_ys > 1) {
        MP_WARN(f, "unsupported format %s, passing through\n", mp_imgfmt_to_name(mpi->imgfmt));
        return false;
    }
    mb_format fmt = {
        .w = mpi->w,
        .h = mpi->h,
        .depth = reg.component_size == 1 ? 8 : 16 - (reg.component_pad < 0 ? -reg.component_pad : 0),
        .cshift_x = reg.chroma_xs,
        .cshift_y = reg.chroma_ys,
        .lsb_pad = reg.component_pad > 0 ? reg.component_pad : 0,
    };
    if (reg.num_planes == 2 && reg.planes[0].num_components == 1 && reg.planes[1].num_components == 2) {
        fmt.nv = 1;
    } else if (!(reg.num_planes == 3 && reg.planes[0].num_components == 1 && reg.planes[1].num_components == 1 &&
                 reg.planes[2].num_components == 1)) {
        MP_WARN(f, "unsupported plane layout %s, passing through\n", mp_imgfmt_to_name(mpi->imgfmt));
        return false;
    }
#if HAVE_VULKAN
    if (mpi->imgfmt == IMGFMT_VULKAN && !p->nozc)
        p->ctx = create_vk(f, mpi, &fmt);
#endif
    if (!p->ctx)
        p->ctx = mb_create(&fmt);
    if (!p->ctx) {
        MP_ERR(f, "could not initialize Vulkan interpolator, passing through\n");
        return false;
    }
    p->disabled = false;
    if (!p->vkframes)
        setup_export(f, mpi, swfmt);
    MP_VERBOSE(f, "interpolating %dx%d %s%s\n", mpi->w, mpi->h, mp_imgfmt_to_name(swfmt),
               p->xframes || p->vkframes ? ", frames stay on the GPU" : "");
    return true;
}

static void enqueue(struct priv *p, struct mp_frame frame, bool gpu)
{
    MP_TARRAY_APPEND(p, p->outq, p->nout, ((struct qent){frame, gpu}));
    if (gpu)
        p->ngpu++;
}

static bool gpu_synced(const struct qent *e)
{
    return ((struct mp_image *)e->frame.data)->imgfmt == IMGFMT_VULKAN;
}

static void finish_oldest_gpu(struct priv *p)
{
    for (int i = 0; i < p->nout; i++) {
        struct qent *e = &p->outq[i];
        if (!e->gpu)
            continue;
        if (p->xframes && !gpu_synced(e)) {
            mb_dmabuf x;
            struct mp_image *ref = e->frame.data;
            struct mp_image *img = mb_retrieve_dmabuf(p->ctx, &x) >= 0 ? drm_image(p, &x, ref) : NULL;
            talloc_free(ref);
            e->frame = img ? MAKE_FRAME(MP_FRAME_VIDEO, img) : MP_NO_FRAME;
        } else if (!gpu_synced(e)) {
            mb_image dst = to_mb(e->frame.data);
            mb_retrieve(p->ctx, &dst);
        }
        e->gpu = false;
        p->ngpu--;
        return;
    }
}

static double display_fps(struct mp_filter *f)
{
    struct priv *p = f->priv;
    m_config_cache_update(p->vo_opts_cache);
    struct mp_vo_opts *vo_opts = p->vo_opts_cache->opts;
    if (vo_opts->display_fps_override > 0)
        return vo_opts->display_fps_override;
    int64_t now = mp_time_ns();
    if (!p->dfps_time || now - p->dfps_time > MP_TIME_S_TO_NS(10)) {
        struct mp_stream_info *info = mp_filter_find_stream_info(f);
        p->dfps = info && info->get_display_fps ? info->get_display_fps(info) : 0;
        p->dfps_time = now;
    }
    return p->dfps;
}

static bool vk_out(struct priv *p)
{
    return p->vkframes && !p->hw_failed;
}

static void enqueue_real(struct priv *p, struct mp_image *r)
{
    if (r->imgfmt == IMGFMT_VULKAN && vk_out(p)) {
        enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, r), false);
        return;
    }
    struct mp_image *m = map_drm(p, r);
    if (m) {
        talloc_free(r);
        enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, m), false);
        return;
    }
    r = download(p, r);
    if (r)
        enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, to_drm(p, r)), false);
}

static void enqueue_ref(struct priv *p, struct mp_image *img, double pts, double dur)
{
    struct mp_image *r = mp_image_new_ref(img);
    if (!r)
        return;
    r->pts = pts;
    r->pkt_duration = dur;
    enqueue_real(p, r);
}

static struct mp_image *submit_out(struct priv *p, struct mp_image *ref, double t)
{
#if HAVE_VULKAN
    if (vk_out(p))
        return submit_vk(p, ref, t);
#endif
    struct mp_image *out = p->xframes ? mp_image_new_dummy_ref(ref) : mp_image_pool_get(p->pool, ref->imgfmt, ref->w, ref->h);
    if (out && mb_submit(p->ctx, t) == 0) {
        mp_image_copy_attributes(out, ref);
        return out;
    }
    talloc_free(out);
    return NULL;
}

static void submit_interp(struct priv *p, struct mp_image *ref, double t, double pts, double dur)
{
    while (mb_pending(p->ctx) >= MB_MAX_PENDING && p->ngpu)
        finish_oldest_gpu(p);
    struct mp_image *out = submit_out(p, ref, t);
    if (!out) {
        enqueue_ref(p, ref, pts, dur);
        return;
    }
    out->pts = pts;
    out->pkt_duration = dur;
    enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, out), true);
}

static bool short_gap(double gap, double dfps)
{
    return dfps > 0 && gap < 1.05 / dfps;
}

static bool pair_ok(struct priv *p, double a, double b)
{
    double gap = b - a;
    return a != MP_NOPTS_VALUE && b != MP_NOPTS_VALUE && gap > 0 && gap <= p->opts->max_gap &&
           (p->opts->max_fps <= 0 || 1.0 / gap <= p->opts->max_fps);
}

static void emit_pair(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    struct mp_image *prev = p->prev;
    double gap = prev ? mpi->pts - prev->pts : 0;
    bool ok = prev && pair_ok(p, prev->pts, mpi->pts);
    double dfps = p->opts->factor ? 0 : display_fps(f);
    int n = p->opts->factor ? p->opts->factor : 2;

    if (dfps > 0) {
        double step = 1.0 / dfps;
        if (!ok || short_gap(gap, dfps)) {
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
    enqueue_real(p, mpi);
}

static void flush_pending(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!p->cur)
        return;
    struct mp_image *b = p->cur;
    p->cur = NULL;
    mb_flush(p->ctx);
    emit_pair(f, b);
}

static void handle_frame(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    if (is_hw(mpi) && p->hw_failed) {
        mpi = download(p, mpi);
        if (!mpi)
            return;
    }
    if (!setup(f, mpi)) {
        mp_image_unrefp(&p->prev);
        mpi = download(p, mpi);
        if (mpi)
            enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, mpi), false);
        return;
    }
    double lp = p->last_pts, gap = mpi->pts - lp, dfps = p->opts->factor ? 0 : display_fps(f);
    p->last_pts = mpi->pts;
    bool fast = lp != MP_NOPTS_VALUE && mpi->pts != MP_NOPTS_VALUE && gap > 0 &&
                (short_gap(gap, dfps) || (p->opts->max_fps > 0 && 1.0 / gap > p->opts->max_fps));
    bool bypass = fast && p->fast;
    p->fast = fast;
    if (bypass) {
        flush_pending(f);
        mp_image_unrefp(&p->prev);
        mpi->pkt_duration = gap;
        enqueue_real(p, mpi);
        return;
    }
    if (p->prev && !fast && !pair_ok(p, lp, mpi->pts)) {
        flush_pending(f);
        mp_image_unrefp(&p->prev);
    }
    if (!p->prev) {
        while (p->ngpu)
            finish_oldest_gpu(p);
        mb_reset(p->ctx);
    }
    if (is_hw(mpi) && !push_gpu(p, mpi)) {
        MP_VERBOSE(f, "cannot import decoder frames, copying them instead\n");
        mpi = download(p, mpi);
        if (!mpi) {
            mp_image_unrefp(&p->prev);
            return;
        }
    }
    if (!is_hw(mpi)) {
        mb_image src = to_mb(mpi);
        mb_push(p->ctx, &src);
    }
    if (!p->prev) {
        emit_pair(f, mpi);
        return;
    }
    struct mp_image *b = p->cur;
    p->cur = mpi;
    if (b)
        emit_pair(f, b);
}

static bool emit_head(struct mp_filter *f, bool block)
{
    struct priv *p = f->priv;
    if (!p->nout)
        return false;
    struct qent *h = &p->outq[0];
    if (h->gpu) {
        if (!block && !gpu_synced(h) && !mb_ready(p->ctx))
            return false;
        finish_oldest_gpu(p);
    }
    struct mp_frame frame = h->frame;
    p->nout--;
    memmove(&p->outq[0], &p->outq[1], p->nout * sizeof(p->outq[0]));
    if (frame.type != MP_FRAME_NONE)
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
    if (!eof_queued && (p->ngpu == 0 || p->ngpu + p->expect <= MB_MAX_PENDING) && p->nout < MAXQ - 8 &&
        mp_pin_out_request_data(p->in)) {
        struct mp_frame frame = mp_pin_out_read(p->in);
        if (frame.type == MP_FRAME_VIDEO) {
            handle_frame(f, frame.data);
        } else if (frame.type != MP_FRAME_NONE) {
            if (frame.type == MP_FRAME_EOF) {
                flush_pending(f);
                mp_image_unrefp(&p->prev);
            }
            enqueue(p, frame, false);
        }
        mp_filter_internal_mark_progress(f);
        return;
    }

    if (want_out && emit_head(f, true))
        mp_filter_internal_mark_progress(f);
}

static const struct mp_filter_info filter = {
    .name = "motionblade",
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
    p->vo_opts_cache = m_config_cache_alloc(p, f->global, &vo_sub_opts);
    p->grid = MP_NOPTS_VALUE;
    p->last_pts = MP_NOPTS_VALUE;
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
    p->nozc = getenv("MOTIONBLADE_NO_ZEROCOPY");
#if HAVE_DRM
    if (!p->nozc) {
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VAAPI, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VAAPI, IMGFMT_P010);
    }
#endif
#if HAVE_VULKAN
    if (!p->nozc) {
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VULKAN, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VULKAN, IMGFMT_P010);
    }
#endif
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

const struct mp_user_filter_entry vf_motionblade = {
    .desc = {
        .description = "motion-compensated frame interpolation",
        .name = "motionblade",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const struct opts){
            .factor = 2,
            .max_fps = 0,
            .max_gap = 0.25,
        },
        .options = opts_list,
    },
    .create = f_create,
};
