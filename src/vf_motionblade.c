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

#include <libavutil/pixfmt.h>

#include "common/common.h"
#include "common/msg.h"
#include "common/tags.h"
#include "filters/f_autoconvert.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_config.h"
#include "options/m_option.h"
#include "options/options.h"
#include "osdep/timer.h"
#include "video/fmt-conversion.h"
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"

#include "video/filter/motionblade/motionblade.h"

#define MAXQ 32
#define RATE_TOL 1.05
#define MIN_SIZE 64

enum { ZC_AUTO, ZC_DRM, ZC_VULKAN, ZC_NO };

struct opts {
    int factor;
    double max_fps;
    double max_gap;
    int zerocopy;
};

struct qent {
    struct mp_frame frame;
    bool gpu, synced;
};

struct hwmap {
    uintptr_t key;
    struct mp_image *img;
};

struct priv {
    struct mp_log *log;
    struct opts *opts;
    struct m_config_cache *vo_opts_cache, *vk_opts_cache;
    struct mp_pin *in;
    struct mp_image_pool *pool, *dlpool;
    mb_ctx *ctx;
    int imgfmt, hwfmt, w, h;
    int factor;
    struct mp_image *prev, *cur;
    struct hwmap maps[2];
    struct qent *outq;
    int nout, ngpu;
    double grid, dfps, last_pts;
    bool fast;
    int64_t dfps_time;
    int expect;
    struct AVBufferRef *xframes;
    uint32_t fourcc;
    bool hw_failed, export_warned;
    struct AVBufferRef *vkframes;
};

static void drop_maps(struct priv *p)
{
    for (int i = 0; i < 2; i++) {
        mp_image_unrefp(&p->maps[i].img);
        p->maps[i].key = 0;
    }
}

static void clear_state(struct priv *p)
{
    mp_image_unrefp(&p->prev);
    mp_image_unrefp(&p->cur);
    drop_maps(p);
    p->grid = MP_NOPTS_VALUE;
    p->last_pts = MP_NOPTS_VALUE;
    p->fast = false;
    p->hw_failed = false;
    p->dfps_time = 0;
    if (p->ctx)
        mb_reset(p->ctx);
    for (int i = 0; i < p->nout; i++)
        mp_frame_unref(&p->outq[i].frame);
    p->nout = 0;
    p->ngpu = 0;
}

static void teardown(struct priv *p)
{
    mb_destroy(p->ctx);
    p->ctx = NULL;
    av_buffer_unref(&p->xframes);
    av_buffer_unref(&p->vkframes);
    drop_maps(p);
}

static void f_reset(struct mp_filter *f)
{
    clear_state(f->priv);
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    clear_state(p);
    teardown(p);
}

static void log_cb(void *opaque, int level, const char *msg)
{
    static const int lev[] = {MSGL_ERR, MSGL_WARN, MSGL_INFO, MSGL_V};
    struct mp_filter *f = opaque;
    MP_MSG(f, lev[level], "%s\n", msg);
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

static bool zc_allows(struct priv *p, int mode)
{
    return p->opts->zerocopy == ZC_AUTO || p->opts->zerocopy == mode;
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

static void setup_export(struct mp_filter *f, struct mp_image *mpi, int swfmt)
{
    struct priv *p = f->priv;
    p->fourcc = swfmt == IMGFMT_NV12   ? DRM_FORMAT_NV12
                : swfmt == IMGFMT_P010 ? DRM_FORMAT_P010
                : swfmt == IMGFMT_420P ? DRM_FORMAT_YUV420
                                       : 0;
    if (!p->fourcc || !zc_allows(p, ZC_DRM))
        return;
    struct mp_hwdec_ctx *hw = mp_filter_load_hwdec_device(f, IMGFMT_DRMPRIME, AV_HWDEVICE_TYPE_DRM);
    if (!hw) {
        MP_VERBOSE(f, "the video output takes no dma-buf frames\n");
        return;
    }
    if (mp_update_av_hw_frames_pool(&p->xframes, hw->av_device_ref, IMGFMT_DRMPRIME, swfmt, mpi->w, mpi->h, false) &&
        mb_export_init(p->ctx) < 0) {
        MP_VERBOSE(f, "dma-buf export is not available on this GPU\n");
        av_buffer_unref(&p->xframes);
    }
}

static struct mp_image *drm_image(struct priv *p, const mb_dmabuf *x, struct mp_image *ref)
{
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
    return NULL;
}

static struct mp_image *map_drm(struct priv *p, struct mp_image *mpi)
{
    if (mpi->imgfmt != IMGFMT_VAAPI || p->hw_failed)
        return NULL;
    struct mp_image *img = NULL;
    AVFrame *av = mp_image_to_av_frame(mpi), *d = av_frame_alloc();
    if (av && d) {
        d->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(d, av, AV_HWFRAME_MAP_READ) >= 0) {
            if (!d->hw_frames_ctx && p->xframes)
                d->hw_frames_ctx = av_buffer_ref(p->xframes);
            img = mp_image_from_av_frame(d);
        }
    }
    av_frame_free(&av);
    av_frame_free(&d);
    if (img)
        mp_image_copy_attributes(img, mpi);
    return img;
}
#else
static void setup_export(struct mp_filter *f, struct mp_image *mpi, int swfmt)
{
}

static struct mp_image *drm_image(struct priv *p, const mb_dmabuf *x, struct mp_image *ref)
{
    return NULL;
}

static struct mp_image *map_drm(struct priv *p, struct mp_image *mpi)
{
    return NULL;
}
#endif

static void free_img(void *opaque)
{
    talloc_free(opaque);
}

static uintptr_t map_key(struct mp_image *mpi)
{
    return (uintptr_t)mpi->planes[3];
}

static void remember_map(struct priv *p, struct mp_image *src, struct mp_image *m)
{
    mp_image_unrefp(&p->maps[1].img);
    p->maps[1] = p->maps[0];
    p->maps[0].key = map_key(src);
    p->maps[0].img = mp_image_new_ref(m);
}

static struct mp_image *find_map(struct priv *p, struct mp_image *src)
{
    if (src->imgfmt != IMGFMT_VAAPI)
        return NULL;
    for (int i = 0; i < 2; i++)
        if (p->maps[i].img && p->maps[i].key == map_key(src))
            return mp_image_new_ref(p->maps[i].img);
    return NULL;
}

static bool push_hw(struct priv *p, struct mp_image *mpi)
{
    struct mp_image *m = map_drm(p, mpi);
    mb_dmabuf_in in = {.release = free_img, .opaque = m};
    int n = 0;
#if HAVE_DRM
    const AVDRMFrameDescriptor *x = m ? (AVDRMFrameDescriptor *)m->planes[0] : NULL;
    for (int i = 0; x && i < x->nb_layers; i++)
        for (int j = 0; j < x->layers[i].nb_planes; j++, n++) {
            if (n >= 2)
                continue;
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
    remember_map(p, mpi, m);
    return true;
}

#if HAVE_VULKAN
#if !defined(FF_API_VULKAN_SYNC_QUEUES) || FF_API_VULKAN_SYNC_QUEUES
#define HAVE_VK_LOCK 1
#else
#define HAVE_VK_LOCK 0
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#if HAVE_VK_LOCK
static void vk_lock(void *opaque, uint32_t qf, uint32_t qi)
{
    AVHWDeviceContext *dc = opaque;
    AVVulkanDeviceContext *vk = dc->hwctx;
    vk->lock_queue(dc, qf, qi);
}

static void vk_unlock(void *opaque, uint32_t qf, uint32_t qi)
{
    AVHWDeviceContext *dc = opaque;
    AVVulkanDeviceContext *vk = dc->hwctx;
    vk->unlock_queue(dc, qf, qi);
}
#endif

static bool vk_can_lock(AVVulkanDeviceContext *vk)
{
#if HAVE_VK_LOCK
    return vk->lock_queue && vk->unlock_queue;
#else
    for (int i = 0; i < vk->nb_enabled_dev_extensions; i++)
        if (!strcmp(vk->enabled_dev_extensions[i], "VK_KHR_internally_synchronized_queues"))
            return true;
    return false;
#endif
}
#pragma GCC diagnostic pop

static mb_ctx *create_vk(struct mp_filter *f, struct mp_image *mpi, const mb_format *fmt, const mb_host *host)
{
    struct priv *p = f->priv;
    AVHWFramesContext *hwfc = (AVHWFramesContext *)mpi->hwctx->data;
    AVHWDeviceContext *dc = hwfc->device_ctx;
    AVVulkanDeviceContext *vk = dc->hwctx;
    int qf = -1;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    for (int i = 0; i < vk->nb_qf && qf < 0; i++)
        if (vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT)
            qf = vk->qf[i].idx;
    for (int i = 0; i < vk->nb_qf && qf < 0; i++)
        if (vk->qf[i].flags & VK_QUEUE_GRAPHICS_BIT)
            qf = vk->qf[i].idx;
#else
    qf = vk->queue_family_comp_index >= 0 ? vk->queue_family_comp_index : vk->queue_family_index;
#endif
    if (qf < 0 || !fmt->nv)
        return NULL;
    if (!vk_can_lock(vk)) {
        MP_WARN(f, "cannot share the player's Vulkan device (no queue locking), copying frames instead\n");
        return NULL;
    }
    mb_vkdev d = {
        .inst = vk->inst,
        .pd = vk->phys_dev,
        .dev = vk->act_dev,
        .qf = qf,
        .features = &vk->device_features,
#if HAVE_VK_LOCK
        .lock = vk_lock,
        .unlock = vk_unlock,
#endif
        .opaque = dc,
    };
    mb_ctx *c = mb_create_ex(fmt, &d, host);
    if (!c)
        return NULL;
    if (!mp_update_av_hw_frames_pool(&p->vkframes, hwfc->device_ref, IMGFMT_VULKAN, mpi->params.hw_subfmt, mpi->w,
                                     mpi->h, false)) {
        mb_destroy(c);
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

extern const struct m_sub_options vulkan_conf;

static const char *vk_device_name(struct priv *p)
{
    m_config_cache_update(p->vk_opts_cache);
    for (int i = 0; vulkan_conf.opts[i].name; i++)
        if (!strcmp(vulkan_conf.opts[i].name, "vulkan-device"))
            return *(char **)((char *)p->vk_opts_cache->opts + vulkan_conf.opts[i].offset);
    return NULL;
}
#endif

static bool is_hw(struct mp_image *mpi)
{
    return IMGFMT_IS_HWACCEL(mpi->imgfmt);
}

static struct mp_image *download(struct priv *p, struct mp_image *mpi)
{
    if (!is_hw(mpi))
        return mpi;
    struct mp_image *sw = mp_image_hw_download(mpi, p->dlpool);
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

static void warn_export(struct priv *p, const char *what)
{
    if (p->export_warned)
        return;
    p->export_warned = true;
    mp_warn(p->log, "%s, falling back to system memory for that frame\n", what);
}

static struct mp_image *to_drm(struct priv *p, struct mp_image *mpi)
{
    mb_dmabuf x;
    mb_image src = to_mb(mpi);
    if (!p->xframes || mpi->imgfmt != p->imgfmt)
        return mpi;
    if (mb_export_image(p->ctx, &src, &x) < 0) {
        warn_export(p, "no free dma-buf output buffer");
        return mpi;
    }
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
    if (p->imgfmt == swfmt && p->hwfmt == mpi->imgfmt && p->w == mpi->w && p->h == mpi->h)
        return p->ctx != NULL;
    flush_pending(f);
    while (p->ngpu)
        finish_oldest_gpu(p);
    mp_image_unrefp(&p->prev);
    p->grid = MP_NOPTS_VALUE;
    teardown(p);
    p->hw_failed = false;
    p->export_warned = false;
    p->imgfmt = swfmt;
    p->hwfmt = mpi->imgfmt;
    p->w = mpi->w;
    p->h = mpi->h;

    if (mpi->w < MIN_SIZE || mpi->h < MIN_SIZE) {
        MP_VERBOSE(f, "%dx%d is smaller than %dx%d, passing through\n", mpi->w, mpi->h, MIN_SIZE, MIN_SIZE);
        return false;
    }
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
    mb_host host = {.log = log_cb, .opaque = f};
#if HAVE_VULKAN
    host.device = vk_device_name(p);
    if (mpi->imgfmt == IMGFMT_VULKAN && zc_allows(p, ZC_VULKAN))
        p->ctx = create_vk(f, mpi, &fmt, &host);
#endif
    if (!p->ctx)
        p->ctx = mb_create_ex(&fmt, NULL, &host);
    if (!p->ctx) {
        MP_ERR(f, "could not initialize Vulkan interpolator, passing through\n");
        return false;
    }
    if (!p->vkframes)
        setup_export(f, mpi, swfmt);
    MP_INFO(f, "interpolating %dx%d %s, %s\n", mpi->w, mpi->h, mp_imgfmt_to_name(swfmt),
            p->vkframes  ? "frames stay on the GPU (Vulkan)"
            : p->xframes ? "frames stay on the GPU (dma-buf)"
                         : "frames go through system memory");
    return true;
}

static void enqueue_synced(struct priv *p, struct mp_frame frame, bool gpu, bool synced)
{
    MP_TARRAY_APPEND(p, p->outq, p->nout, ((struct qent){frame, gpu, synced}));
    if (gpu)
        p->ngpu++;
}

static void enqueue(struct priv *p, struct mp_frame frame, bool gpu)
{
    enqueue_synced(p, frame, gpu, false);
}

static void finish_oldest_gpu(struct priv *p)
{
    for (int i = 0; i < p->nout; i++) {
        struct qent *e = &p->outq[i];
        if (!e->gpu)
            continue;
        if (p->xframes && !e->synced) {
            mb_dmabuf x;
            struct mp_image *ref = e->frame.data;
            struct mp_image *img = mb_retrieve_dmabuf(p->ctx, &x) >= 0 ? drm_image(p, &x, ref) : NULL;
            if (img) {
                talloc_free(ref);
            } else {
                warn_export(p, "could not export an interpolated frame");
                img = download(p, ref);
            }
            e->frame = img ? MAKE_FRAME(MP_FRAME_VIDEO, img) : MP_NO_FRAME;
        } else if (!e->synced) {
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
    if (p->dfps <= 0 || now - p->dfps_time > MP_TIME_S_TO_NS(10)) {
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
    if (p->xframes) {
        struct mp_image *m = find_map(p, r);
        if (!m)
            m = map_drm(p, r);
        if (m) {
            mp_image_copy_attributes(m, r);
            talloc_free(r);
            enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, m), false);
            return;
        }
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

static struct mp_image *submit_out(struct priv *p, struct mp_image *ref, double t, bool *synced)
{
    *synced = vk_out(p);
#if HAVE_VULKAN
    if (*synced)
        return submit_vk(p, ref, t);
#endif
    struct mp_image *out = p->xframes ? mp_image_new_dummy_ref(ref) : mp_image_pool_get(p->pool, p->imgfmt, p->w, p->h);
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
    bool synced;
    struct mp_image *out = submit_out(p, ref, t, &synced);
    if (!out) {
        enqueue_ref(p, ref, pts, dur);
        return;
    }
    out->pts = pts;
    out->pkt_duration = dur;
    enqueue_synced(p, MAKE_FRAME(MP_FRAME_VIDEO, out), true, synced);
}

static bool short_gap(double gap, double dfps)
{
    return dfps > 0 && gap < RATE_TOL / dfps;
}

static bool no_interp(struct priv *p, double gap, double dfps)
{
    if (dfps <= 0)
        return false;
    return p->factor == 0 ? short_gap(gap, dfps) : (int)(dfps * gap * RATE_TOL) < 2;
}

static bool pair_ok(struct priv *p, double a, double b)
{
    double gap = b - a;
    return a != MP_NOPTS_VALUE && b != MP_NOPTS_VALUE && gap > 0 &&
           (p->opts->max_gap <= 0 || gap <= p->opts->max_gap) &&
           (p->opts->max_fps <= 0 || 1.0 / gap <= p->opts->max_fps);
}

static void emit_pair(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    struct mp_image *prev = p->prev;
    double gap = prev ? mpi->pts - prev->pts : 0;
    bool ok = prev && pair_ok(p, prev->pts, mpi->pts);
    double rate = display_fps(f);

    if (p->factor == 0 && rate > 0) {
        double step = 1.0 / rate;
        if (!ok || short_gap(gap, rate)) {
            enqueue_ref(p, mpi, mpi->pts, gap > 0 ? gap : step);
            p->grid = mpi->pts + step;
        } else {
            int per = (int)(gap / step + 0.5), k = (per + MB_MAX_PENDING) / (MB_MAX_PENDING + 1);
            if (k > 1)
                step *= k;
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

    int n = p->factor > 0 ? p->factor : 2;
    if (ok && rate > 0)
        n = MPMIN(n, (int)(rate * gap * RATE_TOL));
    if (ok && n > 1) {
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

static void check_ctx(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!p->ctx || mb_ok(p->ctx))
        return;
    MP_ERR(f, "Vulkan interpolation failed, passing through\n");
    teardown(p);
    for (int i = 0; i < p->nout; i++) {
        struct qent *e = &p->outq[i];
        if (!e->gpu)
            continue;
        mp_frame_unref(&e->frame);
        e->frame = MP_NO_FRAME;
        e->gpu = false;
    }
    p->ngpu = 0;
    mp_image_unrefp(&p->prev);
    mp_image_unrefp(&p->cur);
}

static void handle_frame(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    if (p->factor < 0) {
        enqueue(p, MAKE_FRAME(MP_FRAME_VIDEO, mpi), false);
        return;
    }
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
    double lp = p->last_pts, gap = mpi->pts - lp, dfps = display_fps(f);
    p->last_pts = mpi->pts;
    bool fast = lp != MP_NOPTS_VALUE && mpi->pts != MP_NOPTS_VALUE && gap > 0 &&
                (no_interp(p, gap, dfps) || (p->opts->max_fps > 0 && 1.0 / gap > p->opts->max_fps));
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
            flush_pending(f);
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
    } else {
        struct mp_image *b = p->cur;
        p->cur = mpi;
        if (b)
            emit_pair(f, b);
    }
    check_ctx(f);
}

static bool emit_head(struct mp_filter *f, bool block)
{
    struct priv *p = f->priv;
    if (!p->nout)
        return false;
    struct qent *h = &p->outq[0];
    if (h->gpu) {
        if (!block && !h->synced && !mb_ready(p->ctx))
            return false;
        finish_oldest_gpu(p);
    }
    struct mp_frame frame = h->frame;
    MP_TARRAY_REMOVE_AT(p->outq, p->nout, 0);
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
    if (!eof_queued && p->ngpu < MB_MAX_PENDING && p->nout < MAXQ - 8 && mp_pin_out_request_data(p->in)) {
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

static const char *factor_str(int v)
{
    return v == 0 ? "auto" : v < 0 ? "off" : mp_tprintf(8, "%d", v);
}

static void set_factor(struct mp_filter *f, int v)
{
    struct priv *p = f->priv;
    if (v == p->factor)
        return;
    p->factor = v;
    p->grid = MP_NOPTS_VALUE;
    p->expect = 1;
    if (v < 0 && p->ctx) {
        flush_pending(f);
        while (p->ngpu)
            finish_oldest_gpu(p);
        mp_image_unrefp(&p->prev);
        teardown(p);
        p->imgfmt = 0;
    }
    MP_VERBOSE(f, "factor %s\n", factor_str(v));
}

static bool f_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    if (cmd->type == MP_FILTER_COMMAND_GET_META) {
        struct mp_tags *t = talloc_zero(NULL, struct mp_tags);
        mp_tags_set_str(t, "factor", factor_str(p->factor));
        *(struct mp_tags **)cmd->res = t;
        return true;
    }
    if (cmd->type != MP_FILTER_COMMAND_TEXT || strcmp(cmd->cmd, "factor") != 0 || !cmd->arg)
        return false;
    static const int cycle[] = {2, 4, 8, 0, -1};
    int v;
    if (!strcmp(cmd->arg, "cycle")) {
        int i = 0;
        while (i < 4 && cycle[i] != p->factor)
            i++;
        v = cycle[(i + 1) % 5];
    } else if (!strcmp(cmd->arg, "auto")) {
        v = 0;
    } else if (!strcmp(cmd->arg, "off")) {
        v = -1;
    } else {
        v = atoi(cmd->arg);
        if (v < 2 || v > 8)
            return false;
    }
    set_factor(f, v);
    return true;
}

static const struct mp_filter_info filter = {
    .name = "motionblade",
    .process = f_process,
    .command = f_command,
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
    p->log = f->log;
    p->opts = talloc_steal(p, options);
    p->factor = p->opts->factor;
    p->pool = mp_image_pool_new(p);
    p->dlpool = mp_image_pool_new(p);
    p->vo_opts_cache = m_config_cache_alloc(p, f->global, &vo_sub_opts);
#if HAVE_VULKAN
    p->vk_opts_cache = m_config_cache_alloc(p, f->global, &vulkan_conf);
#endif
    p->grid = MP_NOPTS_VALUE;
    p->last_pts = MP_NOPTS_VALUE;
    p->expect = 1;

    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv) {
        talloc_free(f);
        return NULL;
    }
    static const enum AVPixelFormat fmts[] = {
        AV_PIX_FMT_YUV420P,   AV_PIX_FMT_NV12,      AV_PIX_FMT_P010,      AV_PIX_FMT_YUV420P10,
        AV_PIX_FMT_YUV422P,   AV_PIX_FMT_YUV444P,   AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_P016,      AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    };
    for (int i = 0; i < MP_ARRAY_SIZE(fmts); i++)
        mp_autoconvert_add_imgfmt(conv, pixfmt2imgfmt(fmts[i]), 0);
#if HAVE_DRM
    if (zc_allows(p, ZC_DRM)) {
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VAAPI, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VAAPI, IMGFMT_P010);
    }
#endif
#if HAVE_VULKAN
    if (zc_allows(p, ZC_VULKAN)) {
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
    {"zerocopy", OPT_CHOICE(zerocopy, {"auto", ZC_AUTO}, {"drm", ZC_DRM}, {"vulkan", ZC_VULKAN}, {"no", ZC_NO})},
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
            .zerocopy = ZC_AUTO,
        },
        .options = opts_list,
    },
    .create = f_create,
};
