#include <math.h>
#include <unistd.h>

#include "config.h"

#if HAVE_DRM
#include <drm_fourcc.h>
#include <libavutil/hwcontext_drm.h>
#endif

#if HAVE_VULKAN
#include <libavutil/hwcontext_vulkan.h>
#endif

#include <libavutil/hwcontext.h>
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

#include "motionblade/engine.h"
#include "motionblade/schedule.h"
#include "motionblade/vulkan.h"

#include "vf_motionblade_pulldown.h"

#define MAX_QUEUE 24
#define DISPLAY_FPS_REFRESH_SECONDS 10

enum { ZEROCOPY_AUTO, ZEROCOPY_DRM, ZEROCOPY_VULKAN, ZEROCOPY_NO };

struct opts {
    int factor;
    double max_fps;
    double max_gap;
    int zerocopy;
    int pulldown;
};

struct queued_frame {
    struct mp_frame frame;
    bool on_gpu;
    bool synced;
    mbvk_export_buffer *export_buffer;
};

struct hw_map {
    uintptr_t key;
    struct mp_image *image;
};

struct priv {
    struct mp_log *log;
    struct opts *opts;
    struct m_config_cache *vo_opts_cache;
    struct m_config_cache *vk_opts_cache;
    struct mp_pin *in;
    struct mp_image_pool *pool;
    struct mp_image_pool *download_pool;
    mb_engine *engine;
    mbvk_backend *vk;
    mb_schedule *schedule;
    int imgfmt;
    int hwfmt;
    int w;
    int h;
    struct hw_map maps[2];
    struct queued_frame *queue;
    int queued;
    int on_gpu;
    double display_fps;
    int64_t display_fps_time;
    struct AVBufferRef *drm_frames;
    uint32_t fourcc;
    bool hw_failed;
    bool export_warned;
    struct AVBufferRef *vk_frames;
};

static bool is_hw(struct mp_image *mpi)
{
    return IMGFMT_IS_HWACCEL(mpi->imgfmt);
}

static bool zerocopy_allows(struct priv *p, int mode)
{
    return p->opts->zerocopy == ZEROCOPY_AUTO || p->opts->zerocopy == mode;
}

static mb_host_image host_image(struct mp_image *mpi)
{
    mb_host_image image = {{NULL}, {0}};
    for (int i = 0; i < 3 && i < mpi->num_planes; i++) {
        image.planes[i] = mpi->planes[i];
        image.strides[i] = mpi->stride[i];
    }
    return image;
}

static void drop_maps(struct priv *p)
{
    for (int i = 0; i < 2; i++) {
        mp_image_unrefp(&p->maps[i].image);
        p->maps[i].key = 0;
    }
}

static struct mp_image *download(struct priv *p, struct mp_image *mpi)
{
    if (!is_hw(mpi)) {
        return mpi;
    }
    struct mp_image *sw = mp_image_hw_download(mpi, p->download_pool);
    talloc_free(mpi);
    return sw;
}

static void enqueue(struct priv *p, struct queued_frame entry)
{
    MP_TARRAY_APPEND(p, p->queue, p->queued, entry);
    if (entry.on_gpu) {
        p->on_gpu++;
    }
}

static void enqueue_plain(struct priv *p, struct mp_frame frame)
{
    enqueue(p, (struct queued_frame){frame, false, false, NULL});
}

static void warn_export(struct priv *p, const char *what)
{
    if (!p->export_warned) {
        p->export_warned = true;
        mp_warn(p->log, "%s, falling back to system memory for that frame\n", what);
    }
}

#if HAVE_DRM
static void free_drm_frame(void *opaque, uint8_t *data)
{
    mbvk_dmabuf_output *out = opaque;
    AVDRMFrameDescriptor *d = (AVDRMFrameDescriptor *)data;
    close(d->objects[0].fd);
    mbvk_export_release(out->pool, out->index);
    av_free(out);
    av_free(d);
}

static void describe_drm(struct priv *p, AVDRMFrameDescriptor *d, const mbvk_dmabuf_output *x)
{
    d->nb_objects = 1;
    d->objects[0].fd = x->fd;
    d->objects[0].size = x->size;
    d->objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
    d->nb_layers = 1;
    d->layers[0].format = p->fourcc;
    d->layers[0].nb_planes = x->plane_count;
    for (int i = 0; i < x->plane_count; i++) {
        d->layers[0].planes[i].object_index = 0;
        d->layers[0].planes[i].offset = x->offsets[i];
        d->layers[0].planes[i].pitch = x->pitches[i];
    }
}

static struct mp_image *drm_image(struct priv *p, const mbvk_dmabuf_output *x, struct mp_image *ref)
{
    AVDRMFrameDescriptor *d = av_mallocz(sizeof(*d));
    mbvk_dmabuf_output *owned = av_mallocz(sizeof(*owned));
    AVFrame *fr = av_frame_alloc();
    struct mp_image *img = NULL;
    if (d && owned && fr) {
        describe_drm(p, d, x);
        *owned = *x;
        fr->format = AV_PIX_FMT_DRM_PRIME;
        fr->width = ref->w;
        fr->height = ref->h;
        fr->data[0] = (uint8_t *)d;
        fr->buf[0] = av_buffer_create((uint8_t *)d, sizeof(*d), free_drm_frame, owned, 0);
    }
    if (fr && fr->buf[0]) {
        fr->hw_frames_ctx = av_buffer_ref(p->drm_frames);
        img = mp_image_from_av_frame(fr);
        av_frame_free(&fr);
        if (img) {
            mp_image_copy_attributes(img, ref);
        }
        return img;
    }
    av_frame_free(&fr);
    av_free(owned);
    av_free(d);
    close(x->fd);
    mbvk_export_release(x->pool, x->index);
    return NULL;
}

static void setup_export(struct mp_filter *f, struct mp_image *mpi, int swfmt, const mb_video_format *format)
{
    struct priv *p = f->priv;
    p->fourcc = swfmt == IMGFMT_NV12   ? DRM_FORMAT_NV12
                : swfmt == IMGFMT_P010 ? DRM_FORMAT_P010
                : swfmt == IMGFMT_420P ? DRM_FORMAT_YUV420
                                       : 0;
    if (!p->fourcc || !zerocopy_allows(p, ZEROCOPY_DRM) || !p->vk) {
        return;
    }
    struct mp_hwdec_ctx *hw = mp_filter_load_hwdec_device(f, IMGFMT_DRMPRIME, AV_HWDEVICE_TYPE_DRM);
    if (!hw) {
        MP_VERBOSE(f, "the video output takes no dma-buf frames\n");
        return;
    }
    if (mp_update_av_hw_frames_pool(&p->drm_frames, hw->av_device_ref, IMGFMT_DRMPRIME, swfmt, mpi->w, mpi->h, false) &&
        mbvk_export_enable(p->vk, format) != MB_SUCCESS) {
        MP_VERBOSE(f, "dma-buf export is not available on this GPU\n");
        av_buffer_unref(&p->drm_frames);
    }
}

static struct mp_image *map_drm(struct priv *p, struct mp_image *mpi)
{
    if (mpi->imgfmt != IMGFMT_VAAPI || p->hw_failed) {
        return NULL;
    }
    struct mp_image *img = NULL;
    AVFrame *av = mp_image_to_av_frame(mpi);
    AVFrame *d = av_frame_alloc();
    if (av && d) {
        d->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(d, av, AV_HWFRAME_MAP_READ) >= 0) {
            if (!d->hw_frames_ctx && p->drm_frames) {
                d->hw_frames_ctx = av_buffer_ref(p->drm_frames);
            }
            img = mp_image_from_av_frame(d);
        }
    }
    av_frame_free(&av);
    av_frame_free(&d);
    return img;
}

static int describe_dmabuf_input(struct mp_image *m, mbvk_dmabuf_input *in)
{
    const AVDRMFrameDescriptor *x = m ? (AVDRMFrameDescriptor *)m->planes[0] : NULL;
    int planes = 0;
    for (int i = 0; x && i < x->nb_layers; i++) {
        for (int j = 0; j < x->layers[i].nb_planes; j++, planes++) {
            if (planes >= 2) {
                continue;
            }
            in->objects[planes] = x->layers[i].planes[j].object_index;
            in->offsets[planes] = x->layers[i].planes[j].offset;
            in->pitches[planes] = x->layers[i].planes[j].pitch;
        }
    }
    for (int i = 0; x && i < x->nb_objects && i < 4; i++) {
        in->fds[i] = x->objects[i].fd;
    }
    in->modifier = x ? x->objects[0].format_modifier : 0;
    return planes;
}
#else
static void setup_export(struct mp_filter *f, struct mp_image *mpi, int swfmt, const mb_video_format *format)
{
}

static struct mp_image *drm_image(struct priv *p, const mbvk_dmabuf_output *x, struct mp_image *ref)
{
    return NULL;
}

static struct mp_image *map_drm(struct priv *p, struct mp_image *mpi)
{
    return NULL;
}

static int describe_dmabuf_input(struct mp_image *m, mbvk_dmabuf_input *in)
{
    return 0;
}
#endif

static void free_image(void *opaque)
{
    talloc_free(opaque);
}

static uintptr_t map_key(struct mp_image *mpi)
{
    return (uintptr_t)mpi->planes[3];
}

static void remember_map(struct priv *p, struct mp_image *source, struct mp_image *mapped)
{
    mp_image_unrefp(&p->maps[1].image);
    p->maps[1] = p->maps[0];
    p->maps[0].key = map_key(source);
    p->maps[0].image = mp_image_new_ref(mapped);
}

static struct mp_image *find_map(struct priv *p, struct mp_image *source)
{
    if (source->imgfmt != IMGFMT_VAAPI) {
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (p->maps[i].image && p->maps[i].key == map_key(source)) {
            return mp_image_new_ref(p->maps[i].image);
        }
    }
    return NULL;
}

static bool push_vaapi(struct priv *p, struct mp_image *mpi)
{
    struct mp_image *mapped = map_drm(p, mpi);
    mbvk_dmabuf_input in = {.release = free_image, .context = mapped};
    int planes = describe_dmabuf_input(mapped, &in);
    mb_device_frame frame = mbvk_dmabuf_frame(&in);
    if (planes != 2 || mb_engine_push_device(p->engine, &frame) != MB_SUCCESS) {
        talloc_free(mapped);
        p->hw_failed = true;
        return false;
    }
    remember_map(p, mpi, mapped);
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
static void lock_queue(void *opaque, uint32_t family, uint32_t index)
{
    AVHWDeviceContext *dc = opaque;
    AVVulkanDeviceContext *vk = dc->hwctx;
    vk->lock_queue(dc, family, index);
}

static void unlock_queue(void *opaque, uint32_t family, uint32_t index)
{
    AVHWDeviceContext *dc = opaque;
    AVVulkanDeviceContext *vk = dc->hwctx;
    vk->unlock_queue(dc, family, index);
}
#endif

static bool can_lock_queues(AVVulkanDeviceContext *vk)
{
#if HAVE_VK_LOCK
    return vk->lock_queue && vk->unlock_queue;
#else
    for (int i = 0; i < vk->nb_enabled_dev_extensions; i++) {
        if (!strcmp(vk->enabled_dev_extensions[i], "VK_KHR_internally_synchronized_queues")) {
            return true;
        }
    }
    return false;
#endif
}
#pragma GCC diagnostic pop

static int compute_queue_family(AVVulkanDeviceContext *vk)
{
    int family = -1;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    for (int i = 0; i < vk->nb_qf && family < 0; i++) {
        if (vk->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
            family = vk->qf[i].idx;
        }
    }
    for (int i = 0; i < vk->nb_qf && family < 0; i++) {
        if (vk->qf[i].flags & VK_QUEUE_GRAPHICS_BIT) {
            family = vk->qf[i].idx;
        }
    }
#else
    family = vk->queue_family_comp_index >= 0 ? vk->queue_family_comp_index : vk->queue_family_index;
#endif
    return family;
}

static mbvk_shared_device shared_device(AVHWDeviceContext *dc, int family)
{
    AVVulkanDeviceContext *vk = dc->hwctx;
    return (mbvk_shared_device){
        .instance = vk->inst,
        .physical_device = vk->phys_dev,
        .device = vk->act_dev,
        .queue_family = (uint32_t)family,
        .features = &vk->device_features,
#if HAVE_VK_LOCK
        .lock_queue = lock_queue,
        .unlock_queue = unlock_queue,
#endif
        .context = dc,
    };
}

struct vulkan_lock {
    AVHWFramesContext *frames;
    AVVkFrame *frame;
};

static bool vulkan_frame_lock(struct mp_image *mpi, mbvk_vulkan_frame *vf, struct vulkan_lock *lock)
{
    lock->frames = (AVHWFramesContext *)mpi->hwctx->data;
    lock->frame = (AVVkFrame *)mpi->planes[0];
    AVVulkanFramesContext *vkfc = lock->frames->hwctx;
    vkfc->lock_frame(lock->frames, lock->frame);
    *vf = (mbvk_vulkan_frame){0};
    while (vf->image_count < 2 && lock->frame->img[vf->image_count]) {
        int i = vf->image_count++;
        vf->images[i] = lock->frame->img[i];
        vf->layouts[i] = lock->frame->layout[i];
        vf->semaphores[i] = lock->frame->sem[i];
        vf->semaphore_values[i] = lock->frame->sem_value[i];
        vf->queue_families[i] = lock->frame->queue_family[i];
    }
    return vf->image_count > 0;
}

static void vulkan_frame_unlock(struct vulkan_lock lock)
{
    AVVulkanFramesContext *vkfc = lock.frames->hwctx;
    vkfc->unlock_frame(lock.frames, lock.frame);
}

static void vulkan_frame_commit(const mbvk_vulkan_frame *vf, struct vulkan_lock lock, VkAccessFlags access)
{
    for (int i = 0; i < vf->image_count; i++) {
        lock.frame->layout[i] = vf->layouts[i];
        lock.frame->sem_value[i] = vf->semaphore_values[i];
        lock.frame->access[i] = access;
    }
    vulkan_frame_unlock(lock);
}

static bool push_vulkan(struct priv *p, struct mp_image *mpi)
{
    mbvk_vulkan_frame vf;
    struct vulkan_lock lock;
    bool ok = vulkan_frame_lock(mpi, &vf, &lock);
    mb_device_frame frame = mbvk_vulkan_frame_ref(&vf);
    ok = ok && mb_engine_push_device(p->engine, &frame) == MB_SUCCESS;
    if (ok) {
        vulkan_frame_commit(&vf, lock, VK_ACCESS_TRANSFER_READ_BIT);
    } else {
        vulkan_frame_unlock(lock);
    }
    return ok;
}

static struct mp_image *submit_vulkan(struct priv *p, struct mp_image *ref, double t)
{
    AVFrame *fr = av_frame_alloc();
    struct mp_image *out = NULL;
    if (fr && av_hwframe_get_buffer(p->vk_frames, fr, 0) >= 0) {
        out = mp_image_from_av_frame(fr);
    }
    av_frame_free(&fr);
    if (!out) {
        return NULL;
    }
    mbvk_vulkan_frame vf;
    struct vulkan_lock lock;
    bool ok = vulkan_frame_lock(out, &vf, &lock);
    mb_device_frame target = mbvk_vulkan_frame_ref(&vf);
    ok = ok && mb_engine_submit_device(p->engine, t, &target) == MB_SUCCESS;
    if (!ok) {
        vulkan_frame_unlock(lock);
        talloc_free(out);
        return NULL;
    }
    vulkan_frame_commit(&vf, lock, VK_ACCESS_TRANSFER_WRITE_BIT);
    mp_image_copy_attributes(out, ref);
    return out;
}

extern const struct m_sub_options vulkan_conf;

static const char *vulkan_device_name(struct priv *p)
{
    m_config_cache_update(p->vk_opts_cache);
    for (int i = 0; vulkan_conf.opts[i].name; i++) {
        if (!strcmp(vulkan_conf.opts[i].name, "vulkan-device")) {
            return *(char **)((char *)p->vk_opts_cache->opts + vulkan_conf.opts[i].offset);
        }
    }
    return NULL;
}
#endif

static void log_message(void *context, mb_log_level level, const char *message)
{
    static const int LEVELS[] = {MSGL_ERR, MSGL_WARN, MSGL_INFO, MSGL_V};
    struct mp_filter *f = context;
    MP_MSG(f, LEVELS[level], "%s\n", message);
}

static mb_logger filter_logger(struct mp_filter *f)
{
    return (mb_logger){log_message, f, MB_LOG_VERBOSE};
}

static mb_engine *create_engine(struct mp_filter *f, const mb_video_format *format, const mbvk_shared_device *shared)
{
    struct priv *p = f->priv;
    mbvk_config config = {.shared = shared, .logger = filter_logger(f)};
#if HAVE_VULKAN
    config.device_name = vulkan_device_name(p);
#endif
    mb_engine_config engine = {.format = *format, .logger = filter_logger(f), .params = mb_params_default()};
    mbvk_backend *vk = NULL;
    mb_engine *created = NULL;
    if (mbvk_backend_create(&config, &engine.backend, &vk) == MB_SUCCESS &&
        mb_engine_create(&engine, &created) == MB_SUCCESS) {
        p->vk = vk;
        return created;
    }
    return NULL;
}

#if HAVE_VULKAN
static mb_engine *create_shared_engine(struct mp_filter *f, struct mp_image *mpi, const mb_video_format *format)
{
    struct priv *p = f->priv;
    AVHWFramesContext *hwfc = (AVHWFramesContext *)mpi->hwctx->data;
    AVHWDeviceContext *dc = hwfc->device_ctx;
    int family = compute_queue_family(dc->hwctx);
    if (family < 0 || !format->interleaved_chroma) {
        return NULL;
    }
    if (!can_lock_queues(dc->hwctx)) {
        MP_WARN(f, "cannot share the player's Vulkan device (no queue locking), copying frames instead\n");
        return NULL;
    }
    mbvk_shared_device shared = shared_device(dc, family);
    mb_engine *engine = create_engine(f, format, &shared);
    if (engine && !mp_update_av_hw_frames_pool(&p->vk_frames, hwfc->device_ref, IMGFMT_VULKAN,
                                               mpi->params.hw_subfmt, mpi->w, mpi->h, false)) {
        mb_engine_destroy(engine);
        p->vk = NULL;
        return NULL;
    }
    return engine;
}
#endif

static void finish_export(struct priv *p, struct queued_frame *e)
{
    struct mp_image *ref = e->frame.data;
    mb_frame_report report;
    mbvk_dmabuf_output out;
    struct mp_image *img = NULL;
    if (mb_engine_finish_device(p->engine, &report) != MB_SUCCESS) {
        mbvk_export_cancel(p->vk, e->export_buffer);
    } else if (mbvk_export_describe(p->vk, e->export_buffer, &out) == MB_SUCCESS) {
        img = drm_image(p, &out, ref);
    }
    e->export_buffer = NULL;
    if (img) {
        talloc_free(ref);
    } else {
        warn_export(p, "could not export an interpolated frame");
        img = download(p, ref);
    }
    e->frame = img ? MAKE_FRAME(MP_FRAME_VIDEO, img) : MP_NO_FRAME;
}

static void finish_host(struct priv *p, struct queued_frame *e)
{
    mb_host_image target = host_image(e->frame.data);
    mb_frame_report report;
    mb_engine_retrieve(p->engine, &target, &report);
}

static void finish_entry(struct priv *p, struct queued_frame *e)
{
    mb_frame_report report;
    if (e->export_buffer) {
        finish_export(p, e);
    } else if (e->synced) {
        mb_engine_finish_device(p->engine, &report);
    } else {
        finish_host(p, e);
    }
    e->on_gpu = false;
    p->on_gpu--;
}

static void finish_oldest_gpu(struct priv *p)
{
    for (int i = 0; i < p->queued; i++) {
        if (p->queue[i].on_gpu) {
            finish_entry(p, &p->queue[i]);
            return;
        }
    }
}

static void teardown(struct priv *p)
{
    mb_engine_destroy(p->engine);
    p->engine = NULL;
    p->vk = NULL;
    av_buffer_unref(&p->drm_frames);
    av_buffer_unref(&p->vk_frames);
    drop_maps(p);
}

static void stop_engine(struct mp_filter *f)
{
    struct priv *p = f->priv;
    mb_schedule_stop(p->schedule);
    while (p->on_gpu) {
        finish_oldest_gpu(p);
    }
    teardown(p);
}

static bool describe_format(struct mp_filter *f, struct mp_image *mpi, int swfmt, mb_video_format *format)
{
    struct mp_regular_imgfmt reg;
    if (!mp_get_regular_imgfmt(&reg, swfmt) || reg.component_type != MP_COMPONENT_TYPE_UINT ||
        reg.component_size > 2 || reg.chroma_xs > 1 || reg.chroma_ys > 1) {
        MP_WARN(f, "unsupported format %s, passing through\n", mp_imgfmt_to_name(mpi->imgfmt));
        return false;
    }
    *format = (mb_video_format){
        .width = mpi->w,
        .height = mpi->h,
        .bit_depth = reg.component_size == 1 ? 8 : 16 - (reg.component_pad < 0 ? -reg.component_pad : 0),
        .chroma_shift_x = reg.chroma_xs,
        .chroma_shift_y = reg.chroma_ys,
        .lsb_padding = reg.component_pad > 0 ? reg.component_pad : 0,
    };
    bool semi_planar = reg.num_planes == 2 && reg.planes[0].num_components == 1 && reg.planes[1].num_components == 2;
    bool planar = reg.num_planes == 3 && reg.planes[0].num_components == 1 && reg.planes[1].num_components == 1 &&
                  reg.planes[2].num_components == 1;
    format->interleaved_chroma = semi_planar;
    if (!semi_planar && !planar) {
        MP_WARN(f, "unsupported plane layout %s, passing through\n", mp_imgfmt_to_name(mpi->imgfmt));
        return false;
    }
    return true;
}

static void announce_path(struct mp_filter *f, struct mp_image *mpi, int swfmt)
{
    struct priv *p = f->priv;
    bool gpu_in = is_hw(mpi);
    bool gpu_out = p->vk_frames || p->drm_frames;
    MP_INFO(f, "interpolating %dx%d %s, %s\n", mpi->w, mpi->h, mp_imgfmt_to_name(swfmt),
            gpu_in && gpu_out ? "frames stay on the GPU"
            : gpu_in          ? "output goes through system memory"
            : gpu_out         ? "decoded frames are copied to the GPU"
                              : "frames go through system memory");
}

static bool same_stream(struct priv *p, struct mp_image *mpi, int swfmt)
{
    return p->imgfmt == swfmt && p->hwfmt == mpi->imgfmt && p->w == mpi->w && p->h == mpi->h;
}

static void remember_stream(struct priv *p, struct mp_image *mpi, int swfmt)
{
    p->hw_failed = false;
    p->export_warned = false;
    p->imgfmt = swfmt;
    p->hwfmt = mpi->imgfmt;
    p->w = mpi->w;
    p->h = mpi->h;
}

static mb_engine *start_engine(struct mp_filter *f, struct mp_image *mpi, const mb_video_format *format)
{
    mb_engine *engine = NULL;
#if HAVE_VULKAN
    struct priv *p = f->priv;
    if (mpi->imgfmt == IMGFMT_VULKAN && zerocopy_allows(p, ZEROCOPY_VULKAN)) {
        engine = create_shared_engine(f, mpi, format);
    }
#endif
    return engine ? engine : create_engine(f, format, NULL);
}

static bool setup(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    int swfmt = is_hw(mpi) ? mpi->params.hw_subfmt : mpi->imgfmt;
    if (same_stream(p, mpi, swfmt)) {
        return p->engine != NULL;
    }
    stop_engine(f);
    remember_stream(p, mpi, swfmt);
    if (mpi->w < MB_MIN_FRAME_SIZE || mpi->h < MB_MIN_FRAME_SIZE) {
        MP_VERBOSE(f, "%dx%d is smaller than %dx%d, passing through\n", mpi->w, mpi->h, MB_MIN_FRAME_SIZE,
                   MB_MIN_FRAME_SIZE);
        return false;
    }
    mb_video_format format;
    if (!describe_format(f, mpi, swfmt, &format)) {
        return false;
    }
    p->engine = start_engine(f, mpi, &format);
    if (!p->engine) {
        MP_ERR(f, "could not initialize Vulkan interpolator, passing through\n");
        return false;
    }
    if (!p->vk_frames) {
        setup_export(f, mpi, swfmt, &format);
    }
    announce_path(f, mpi, swfmt);
    return true;
}

static bool vulkan_output(struct priv *p)
{
    return p->vk_frames && !p->hw_failed;
}

static struct mp_image *export_frame(struct priv *p, struct mp_image *mpi)
{
    mbvk_export_buffer *buffer;
    if (!p->drm_frames || mpi->imgfmt != p->imgfmt) {
        return mpi;
    }
    mb_host_image source = host_image(mpi);
    if (mbvk_export_host_image(p->vk, &source, &buffer) != MB_SUCCESS) {
        warn_export(p, "no free dma-buf output buffer");
        return mpi;
    }
    mbvk_dmabuf_output out;
    if (mbvk_export_describe(p->vk, buffer, &out) != MB_SUCCESS) {
        return mpi;
    }
    struct mp_image *img = drm_image(p, &out, mpi);
    if (!img) {
        return mpi;
    }
    talloc_free(mpi);
    return img;
}

static void enqueue_real(struct priv *p, struct mp_image *r)
{
    if (r->imgfmt == IMGFMT_VULKAN && vulkan_output(p)) {
        enqueue_plain(p, MAKE_FRAME(MP_FRAME_VIDEO, r));
        return;
    }
    if (p->drm_frames) {
        struct mp_image *mapped = find_map(p, r);
        if (!mapped) {
            mapped = map_drm(p, r);
        }
        if (mapped) {
            mp_image_copy_attributes(mapped, r);
            talloc_free(r);
            enqueue_plain(p, MAKE_FRAME(MP_FRAME_VIDEO, mapped));
            return;
        }
    }
    r = download(p, r);
    if (r) {
        enqueue_plain(p, MAKE_FRAME(MP_FRAME_VIDEO, export_frame(p, r)));
    }
}

static void emit_real(struct priv *p, const mb_action *a)
{
    struct mp_image *r = mp_image_new_ref(a->frame);
    if (!r) {
        return;
    }
    r->pts = a->pts;
    if (a->duration >= 0) {
        r->pkt_duration = a->duration;
    }
    enqueue_real(p, r);
}

static void drop_entry(struct priv *p, struct queued_frame *e)
{
    if (e->export_buffer) {
        mbvk_export_cancel(p->vk, e->export_buffer);
        e->export_buffer = NULL;
    }
    mp_frame_unref(&e->frame);
    e->on_gpu = false;
}

static struct mp_image *submit_host_or_export(struct priv *p, struct mp_image *ref, double t,
                                              mbvk_export_buffer **buffer)
{
    *buffer = NULL;
    if (p->drm_frames && mbvk_export_acquire(p->vk, buffer) != MB_SUCCESS) {
        return NULL;
    }
    struct mp_image *out = p->drm_frames ? mp_image_new_dummy_ref(ref)
                                         : mp_image_pool_get(p->pool, p->imgfmt, p->w, p->h);
    mb_device_frame target = *buffer ? mbvk_export_target(*buffer) : (mb_device_frame){0};
    mb_status status = MB_ERROR_NO_MEMORY;
    if (out) {
        status = *buffer ? mb_engine_submit_device(p->engine, t, &target) : mb_engine_submit(p->engine, t);
    }
    if (status != MB_SUCCESS) {
        if (*buffer) {
            mbvk_export_cancel(p->vk, *buffer);
            *buffer = NULL;
        }
        talloc_free(out);
        return NULL;
    }
    mp_image_copy_attributes(out, ref);
    return out;
}

static void emit_interpolated(struct priv *p, const mb_action *a)
{
    while (mb_engine_pending(p->engine) >= MB_PENDING_LIMIT && p->on_gpu) {
        finish_oldest_gpu(p);
    }
    struct mp_image *ref = a->frame;
    mbvk_export_buffer *buffer = NULL;
    struct mp_image *out = NULL;
    bool synced = vulkan_output(p);
#if HAVE_VULKAN
    if (synced) {
        out = submit_vulkan(p, ref, a->t);
    }
#endif
    if (!synced) {
        out = submit_host_or_export(p, ref, a->t, &buffer);
    }
    if (!out) {
        emit_real(p, a);
        return;
    }
    out->pts = a->pts;
    out->pkt_duration = a->duration;
    enqueue(p, (struct queued_frame){MAKE_FRAME(MP_FRAME_VIDEO, out), true, synced, buffer});
}

static bool push_frame(struct mp_filter *f, mb_action *a)
{
    struct priv *p = f->priv;
    struct mp_image *mpi = a->frame;
    if (is_hw(mpi)) {
        bool pushed = mpi->imgfmt == IMGFMT_VAAPI ? push_vaapi(p, mpi) : false;
#if HAVE_VULKAN
        pushed = pushed || (mpi->imgfmt != IMGFMT_VAAPI && p->vk_frames && push_vulkan(p, mpi));
#endif
        if (pushed) {
            return true;
        }
        p->hw_failed = true;
        MP_VERBOSE(f, "cannot import decoder frames, copying them instead\n");
        mpi = download(p, mpi);
        a->frame = mpi;
        if (!mpi) {
            return false;
        }
    }
    mb_host_image source = host_image(mpi);
    mb_engine_push(p->engine, &source);
    return true;
}

static bool needs_engine(mb_action_kind kind)
{
    return kind == MB_ACTION_RESET_ENGINE || kind == MB_ACTION_PUSH || kind == MB_ACTION_END_STREAM ||
           kind == MB_ACTION_EMIT_INTERPOLATED;
}

static bool run_action(void *context, mb_action *a)
{
    struct mp_filter *f = context;
    struct priv *p = f->priv;
    if (needs_engine(a->kind) && !p->engine) {
        return false;
    }
    switch (a->kind) {
    case MB_ACTION_PASS:
        enqueue_plain(p, MAKE_FRAME(MP_FRAME_VIDEO, mp_image_new_ref(a->frame)));
        return true;
    case MB_ACTION_DRAIN:
        while (p->on_gpu) {
            finish_oldest_gpu(p);
        }
        return true;
    case MB_ACTION_RESET_ENGINE:
        mb_engine_reset(p->engine);
        return true;
    case MB_ACTION_PUSH:
        return push_frame(f, a);
    case MB_ACTION_END_STREAM:
        mb_engine_end_stream(p->engine);
        return true;
    case MB_ACTION_EMIT_REAL:
        emit_real(p, a);
        return true;
    case MB_ACTION_EMIT_INTERPOLATED:
        emit_interpolated(p, a);
        return true;
    case MB_ACTION_RELEASE:
        talloc_free(a->frame);
        return true;
    }
    return false;
}

static double display_fps(struct mp_filter *f)
{
    struct priv *p = f->priv;
    m_config_cache_update(p->vo_opts_cache);
    struct mp_vo_opts *vo_opts = p->vo_opts_cache->opts;
    if (vo_opts->display_fps_override > 0) {
        return vo_opts->display_fps_override;
    }
    int64_t now = mp_time_ns();
    if (p->display_fps <= 0 || now - p->display_fps_time > MP_TIME_S_TO_NS(DISPLAY_FPS_REFRESH_SECONDS)) {
        struct mp_stream_info *info = mp_filter_find_stream_info(f);
        p->display_fps = info && info->get_display_fps ? info->get_display_fps(info) : 0;
        p->display_fps_time = now;
    }
    return p->display_fps;
}

static void check_engine(struct mp_filter *f)
{
    struct priv *p = f->priv;
    if (!p->engine || mb_engine_is_healthy(p->engine)) {
        return;
    }
    MP_ERR(f, "Vulkan interpolation failed, passing through\n");
    for (int i = 0; i < p->queued; i++) {
        if (p->queue[i].on_gpu) {
            drop_entry(p, &p->queue[i]);
        }
    }
    p->on_gpu = 0;
    mb_schedule_forget_frames(p->schedule);
    teardown(p);
}

static double pts_of(struct mp_image *mpi)
{
    return mpi->pts == MP_NOPTS_VALUE ? NAN : mpi->pts;
}

static void handle_frame(struct mp_filter *f, struct mp_image *mpi)
{
    struct priv *p = f->priv;
    if (mb_schedule_factor(p->schedule) == MB_FACTOR_OFF) {
        mb_schedule_frame(p->schedule, (mb_timed_frame){mpi, pts_of(mpi)}, display_fps(f));
        return;
    }
    if (is_hw(mpi) && p->hw_failed) {
        mpi = download(p, mpi);
        if (!mpi) {
            return;
        }
    }
    if (!setup(f, mpi)) {
        mb_schedule_forget_frames(p->schedule);
        mpi = download(p, mpi);
        if (mpi) {
            enqueue_plain(p, MAKE_FRAME(MP_FRAME_VIDEO, mpi));
        }
        return;
    }
    mb_schedule_frame(p->schedule, (mb_timed_frame){mpi, pts_of(mpi)}, display_fps(f));
    check_engine(f);
}

static bool emit_head(struct mp_filter *f, bool block)
{
    struct priv *p = f->priv;
    if (!p->queued) {
        return false;
    }
    struct queued_frame *head = &p->queue[0];
    if (head->on_gpu) {
        if (!block && !head->synced && !mb_engine_output_ready(p->engine)) {
            return false;
        }
        finish_entry(p, head);
    }
    struct mp_frame frame = head->frame;
    MP_TARRAY_REMOVE_AT(p->queue, p->queued, 0);
    if (frame.type != MP_FRAME_NONE) {
        mp_pin_in_write(f->ppins[1], frame);
    }
    return true;
}

static bool eof_queued(struct priv *p)
{
    return p->queued && p->queue[p->queued - 1].frame.type == MP_FRAME_EOF;
}

static void read_input(struct mp_filter *f)
{
    struct priv *p = f->priv;
    struct mp_frame frame = mp_pin_out_read(p->in);
    if (frame.type == MP_FRAME_VIDEO) {
        handle_frame(f, frame.data);
    } else if (frame.type != MP_FRAME_NONE) {
        if (frame.type == MP_FRAME_EOF && p->engine) {
            mb_schedule_end(p->schedule, display_fps(f));
        }
        enqueue_plain(p, frame);
    }
}

static void f_process(struct mp_filter *f)
{
    struct priv *p = f->priv;
    bool want_out = mp_pin_in_needs_data(f->ppins[1]);
    if (want_out && emit_head(f, false)) {
        mp_filter_internal_mark_progress(f);
        return;
    }
    if (!eof_queued(p) && p->on_gpu < MB_PENDING_LIMIT && p->queued < MAX_QUEUE &&
        mp_pin_out_request_data(p->in)) {
        read_input(f);
        mp_filter_internal_mark_progress(f);
        return;
    }
    if (want_out && emit_head(f, true)) {
        mp_filter_internal_mark_progress(f);
    }
}

static void clear_queue(struct priv *p)
{
    for (int i = 0; i < p->queued; i++) {
        drop_entry(p, &p->queue[i]);
    }
    p->queued = 0;
    p->on_gpu = 0;
}

static void f_reset(struct mp_filter *f)
{
    struct priv *p = f->priv;
    mb_schedule_reset(p->schedule);
    drop_maps(p);
    p->hw_failed = false;
    p->display_fps_time = 0;
    if (p->engine) {
        mb_engine_reset(p->engine);
    }
    clear_queue(p);
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    f_reset(f);
    teardown(p);
    mb_schedule_destroy(p->schedule);
}

static const char *factor_name(int v)
{
    return v == MB_FACTOR_AUTO ? "auto" : v == MB_FACTOR_OFF ? "off" : mp_tprintf(8, "%d", v);
}

static void set_factor(struct mp_filter *f, int v)
{
    struct priv *p = f->priv;
    if (v == mb_schedule_factor(p->schedule)) {
        return;
    }
    bool stopping = v == MB_FACTOR_OFF && p->engine;
    mb_schedule_set_factor(p->schedule, v);
    if (stopping) {
        teardown(p);
        p->imgfmt = 0;
    }
    MP_VERBOSE(f, "factor %s\n", factor_name(v));
}

static bool parse_factor(const char *arg, int current, int *factor)
{
    static const int CYCLE[] = {2, 4, MB_MAX_FACTOR, MB_FACTOR_AUTO, MB_FACTOR_OFF};
    int n = MP_ARRAY_SIZE(CYCLE);
    if (!strcmp(arg, "cycle")) {
        int i = 0;
        while (i < n - 1 && CYCLE[i] != current) {
            i++;
        }
        *factor = CYCLE[(i + 1) % n];
    } else if (!strcmp(arg, "auto")) {
        *factor = MB_FACTOR_AUTO;
    } else if (!strcmp(arg, "off")) {
        *factor = MB_FACTOR_OFF;
    } else {
        *factor = atoi(arg);
        return *factor >= 2 && *factor <= MB_MAX_FACTOR;
    }
    return true;
}

static bool f_command(struct mp_filter *f, struct mp_filter_command *cmd)
{
    struct priv *p = f->priv;
    int current = mb_schedule_factor(p->schedule);
    if (cmd->type == MP_FILTER_COMMAND_GET_META) {
        struct mp_tags *t = talloc_zero(NULL, struct mp_tags);
        mp_tags_set_str(t, "factor", factor_name(current));
        *(struct mp_tags **)cmd->res = t;
        return true;
    }
    int factor;
    if (cmd->type != MP_FILTER_COMMAND_TEXT || strcmp(cmd->cmd, "factor") != 0 || !cmd->arg ||
        !parse_factor(cmd->arg, current, &factor)) {
        return false;
    }
    set_factor(f, factor);
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

static void add_formats(struct priv *p, struct mp_autoconvert *conv)
{
    static const enum AVPixelFormat FORMATS[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_P016, AV_PIX_FMT_YUV420P16,
        AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
    };
    for (int i = 0; i < MP_ARRAY_SIZE(FORMATS); i++) {
        mp_autoconvert_add_imgfmt(conv, pixfmt2imgfmt(FORMATS[i]), 0);
    }
#if HAVE_DRM
    if (zerocopy_allows(p, ZEROCOPY_DRM)) {
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VAAPI, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VAAPI, IMGFMT_P010);
    }
#endif
#if HAVE_VULKAN
    if (zerocopy_allows(p, ZEROCOPY_VULKAN)) {
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VULKAN, IMGFMT_NV12);
        mp_autoconvert_add_imgfmt(conv, IMGFMT_VULKAN, IMGFMT_P010);
    }
#endif
}

static bool add_pulldown(struct mp_filter *f)
{
    struct priv *p = f->priv;
    struct mp_filter *pulldown = mb_pulldown_filter_create(f);
    if (!pulldown) {
        return false;
    }
    mp_pin_connect(pulldown->pins[0], p->in);
    p->in = pulldown->pins[1];
    return true;
}

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
    p->pool = mp_image_pool_new(p);
    p->download_pool = mp_image_pool_new(p);
    p->vo_opts_cache = m_config_cache_alloc(p, f->global, &vo_sub_opts);
#if HAVE_VULKAN
    p->vk_opts_cache = m_config_cache_alloc(p, f->global, &vulkan_conf);
#endif
    mb_schedule_config config = {p->opts->factor, p->opts->max_fps, p->opts->max_gap, MB_PENDING_LIMIT};
    p->schedule = mb_schedule_create(&config, (mb_action_sink){run_action, f});
    struct mp_autoconvert *conv = mp_autoconvert_create(f);
    if (!conv || !p->schedule) {
        talloc_free(f);
        return NULL;
    }
    add_formats(p, conv);
    mp_pin_connect(conv->f->pins[0], f->ppins[0]);
    p->in = conv->f->pins[1];
    if (p->opts->pulldown && !add_pulldown(f)) {
        talloc_free(f);
        return NULL;
    }
    return f;
}

#define OPT_BASE_STRUCT struct opts
static const m_option_t opts_list[] = {
    {"factor", OPT_CHOICE(factor, {"auto", MB_FACTOR_AUTO}), M_RANGE(2, MB_MAX_FACTOR)},
    {"max-fps", OPT_DOUBLE(max_fps), M_RANGE(0, 1000)},
    {"max-gap", OPT_DOUBLE(max_gap), M_RANGE(0, 10)},
    {"zerocopy", OPT_CHOICE(zerocopy, {"auto", ZEROCOPY_AUTO}, {"drm", ZEROCOPY_DRM}, {"vulkan", ZEROCOPY_VULKAN},
                            {"no", ZEROCOPY_NO})},
    {"pulldown", OPT_CHOICE(pulldown, {"no", 0}, {"auto", 1})},
    {0},
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
            .zerocopy = ZEROCOPY_AUTO,
            .pulldown = 1,
        },
        .options = opts_list,
    },
    .create = f_create,
};
