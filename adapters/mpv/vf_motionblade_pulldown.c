#include "vf_motionblade_pulldown.h"

#include <math.h>

#include "common/common.h"
#include "common/msg.h"
#include "filters/f_lavfi.h"
#include "filters/f_utils.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "video/img_format.h"
#include "video/mp_image.h"

#include "motionblade/pulldown.h"

struct pulldown_priv {
    struct mp_subfilter sub;
    mb_pulldown_detector *detector;
    struct mp_image *previous;
    mb_pulldown wanted;
    mb_pulldown active;
    bool observed;
    bool failed;
};

static bool luma_plane(struct mp_image *mpi, mb_luma_plane *plane)
{
    struct mp_regular_imgfmt reg;
    if (IMGFMT_IS_HWACCEL(mpi->imgfmt) || !mp_get_regular_imgfmt(&reg, mpi->imgfmt) ||
        reg.component_type != MP_COMPONENT_TYPE_UINT || reg.component_size > 2) {
        return false;
    }
    int bits = reg.component_size * 8 - (reg.component_pad < 0 ? -reg.component_pad : 0);
    *plane = (mb_luma_plane){
        .data = mpi->planes[0],
        .stride = mpi->stride[0],
        .width = mpi->w,
        .height = mpi->h,
        .bytes_per_sample = reg.component_size,
        .max_value = (1 << bits) - 1,
    };
    return true;
}

static bool same_layout(struct mp_image *a, struct mp_image *b)
{
    return a && a->imgfmt == b->imgfmt && a->w == b->w && a->h == b->h;
}

static mb_pulldown observe(struct pulldown_priv *p, struct mp_image *mpi)
{
    mb_luma_plane current;
    if (p->failed || !luma_plane(mpi, &current)) {
        mp_image_unrefp(&p->previous);
        return MB_PULLDOWN_NONE;
    }
    mb_luma_plane previous;
    if (same_layout(p->previous, mpi) && luma_plane(p->previous, &previous)) {
        mb_pulldown_push(p->detector, mb_field_motion_measure(&previous, &current));
    } else {
        mb_pulldown_forget(p->detector);
    }
    mp_image_unrefp(&p->previous);
    p->previous = mp_image_new_ref(mpi);
    return mb_pulldown_state(p->detector);
}

struct dropper_priv {
    struct mp_image *shown;
    struct mp_image *pending;
    double motion_shown;
    double motion_pending;
    bool eof_waiting;
};

static double motion_between(struct mp_image *a, struct mp_image *b)
{
    mb_luma_plane pa, pb;
    if (!a || !same_layout(a, b) || !luma_plane(a, &pa) || !luma_plane(b, &pb)) {
        return NAN;
    }
    return mb_frame_motion(&pa, &pb);
}

static void forget_frames(struct dropper_priv *p)
{
    mp_image_unrefp(&p->shown);
    mp_image_unrefp(&p->pending);
    p->motion_shown = NAN;
    p->motion_pending = NAN;
}

static struct mp_image *pass_pending(struct dropper_priv *p, struct mp_image *next, double motion_after)
{
    struct mp_image *out = p->pending;
    mp_image_unrefp(&p->shown);
    p->shown = mp_image_new_ref(out);
    p->motion_shown = p->motion_pending;
    p->pending = next;
    p->motion_pending = motion_after;
    return out;
}

static struct mp_image *accept_frame(struct dropper_priv *p, struct mp_image *next)
{
    if (!p->pending) {
        p->pending = next;
        p->motion_pending = motion_between(p->shown, next);
        return NULL;
    }
    double motion_after = motion_between(p->pending, next);
    if (!mb_frame_repeats(p->motion_shown, p->motion_pending, motion_after)) {
        return pass_pending(p, next, motion_after);
    }
    talloc_free(p->pending);
    p->pending = next;
    p->motion_pending = motion_between(p->shown, next);
    return NULL;
}

static void flush_at_eof(struct mp_filter *f)
{
    struct dropper_priv *p = f->priv;
    mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, p->pending));
    p->pending = NULL;
    forget_frames(p);
    p->eof_waiting = true;
}

static void dropper_process(struct mp_filter *f)
{
    struct dropper_priv *p = f->priv;
    if (p->eof_waiting) {
        if (mp_pin_in_needs_data(f->ppins[1])) {
            mp_pin_in_write(f->ppins[1], MP_EOF_FRAME);
            p->eof_waiting = false;
        }
        return;
    }
    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0])) {
        return;
    }
    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (frame.type == MP_FRAME_VIDEO) {
        struct mp_image *out = accept_frame(p, frame.data);
        if (out) {
            mp_pin_in_write(f->ppins[1], MAKE_FRAME(MP_FRAME_VIDEO, out));
        } else {
            mp_pin_out_request_data(f->ppins[0]);
            mp_filter_internal_mark_progress(f);
        }
    } else if (frame.type == MP_FRAME_EOF && p->pending) {
        flush_at_eof(f);
    } else {
        mp_pin_in_write(f->ppins[1], frame);
    }
}

static void dropper_reset(struct mp_filter *f)
{
    struct dropper_priv *p = f->priv;
    forget_frames(p);
    p->eof_waiting = false;
}

static const struct mp_filter_info dropper_filter = {
    .name = "motionblade_repeats",
    .priv_size = sizeof(struct dropper_priv),
    .process = dropper_process,
    .reset = dropper_reset,
    .destroy = dropper_reset,
};

static struct mp_filter *create_dropper(struct mp_filter *parent)
{
    struct mp_filter *f = mp_filter_create(parent, &dropper_filter);
    if (!f) {
        return NULL;
    }
    struct dropper_priv *p = f->priv;
    p->motion_shown = NAN;
    p->motion_pending = NAN;
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    return f;
}

static const char *graph_for(mb_pulldown pulldown)
{
    return pulldown == MB_PULLDOWN_TOP_FIRST
               ? "fieldmatch=order=tff:combmatch=full,yadif=deint=interlaced,decimate"
               : "fieldmatch=order=bff:combmatch=full,yadif=deint=interlaced,decimate";
}

static struct mp_filter *create_restorer(struct mp_filter *parent, mb_pulldown pulldown)
{
    struct mp_filter *restorer = mp_bidir_dummy_filter_create(parent);
    if (!restorer) {
        return NULL;
    }
    struct mp_lavfi *graph = mp_lavfi_create_graph(restorer, MP_FRAME_VIDEO, true, NULL, NULL, graph_for(pulldown));
    struct mp_filter *dropper = create_dropper(restorer);
    if (!graph || !dropper) {
        talloc_free(restorer);
        return NULL;
    }
    struct mp_filter *chain[] = {graph->f, dropper};
    mp_chain_filters(restorer->ppins[0], restorer->ppins[1], chain, 2);
    return restorer;
}

static void activate(struct mp_filter *f, mb_pulldown pulldown)
{
    struct pulldown_priv *p = f->priv;
    p->active = pulldown;
    if (pulldown == MB_PULLDOWN_NONE) {
        MP_INFO(f, "film pulldown ended\n");
        return;
    }
    struct mp_filter *restorer = create_restorer(f, pulldown);
    if (!restorer) {
        MP_ERR(f, "could not remove film pulldown\n");
        p->failed = true;
        p->active = MB_PULLDOWN_NONE;
        return;
    }
    p->sub.filter = restorer;
    MP_INFO(f, "film pulldown found (%s field first), restoring the original frames\n",
            pulldown == MB_PULLDOWN_TOP_FIRST ? "top" : "bottom");
}

static void pulldown_process(struct mp_filter *f)
{
    struct pulldown_priv *p = f->priv;
    if (!mp_subfilter_read(&p->sub)) {
        return;
    }
    if (p->sub.frame.type == MP_FRAME_VIDEO && !p->observed) {
        p->wanted = observe(p, p->sub.frame.data);
        p->observed = true;
    }
    if (p->wanted != p->active) {
        if (!mp_subfilter_drain_destroy(&p->sub)) {
            return;
        }
        activate(f, p->wanted);
        p->wanted = p->active;
    }
    mp_subfilter_continue(&p->sub);
    p->observed = p->sub.frame.type != MP_FRAME_NONE;
}

static void pulldown_reset(struct mp_filter *f)
{
    struct pulldown_priv *p = f->priv;
    mp_subfilter_reset(&p->sub);
    mp_image_unrefp(&p->previous);
    mb_pulldown_forget(p->detector);
    p->wanted = p->active;
    p->observed = false;
}

static void pulldown_destroy(struct mp_filter *f)
{
    struct pulldown_priv *p = f->priv;
    mp_subfilter_reset(&p->sub);
    TA_FREEP(&p->sub.filter);
    mp_image_unrefp(&p->previous);
    mb_pulldown_destroy(p->detector);
}

static const struct mp_filter_info pulldown_filter = {
    .name = "motionblade_pulldown",
    .priv_size = sizeof(struct pulldown_priv),
    .process = pulldown_process,
    .reset = pulldown_reset,
    .destroy = pulldown_destroy,
};

struct mp_filter *mb_pulldown_filter_create(struct mp_filter *parent)
{
    struct mp_filter *f = mp_filter_create(parent, &pulldown_filter);
    if (!f) {
        return NULL;
    }
    struct pulldown_priv *p = f->priv;
    p->detector = mb_pulldown_create();
    if (!p->detector) {
        talloc_free(f);
        return NULL;
    }
    p->sub.in = mp_filter_add_pin(f, MP_PIN_IN, "in");
    p->sub.out = mp_filter_add_pin(f, MP_PIN_OUT, "out");
    return f;
}
