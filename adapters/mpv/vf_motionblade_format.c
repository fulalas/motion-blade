#include "vf_motionblade_format.h"

#include "video/img_format.h"
#include "video/mp_image.h"

static bool plain_planes(const struct mp_regular_imgfmt *reg, bool *interleaved)
{
    bool luma = reg->planes[0].num_components == 1;
    *interleaved = reg->num_planes == 2 && luma && reg->planes[1].num_components == 2;
    bool planar = reg->num_planes == 3 && luma && reg->planes[1].num_components == 1 &&
                  reg->planes[2].num_components == 1;
    return *interleaved || planar;
}

static bool integer_samples(int imgfmt, struct mp_regular_imgfmt *reg)
{
    return mp_get_regular_imgfmt(reg, imgfmt) && reg->component_type == MP_COMPONENT_TYPE_UINT &&
           reg->component_size <= 2;
}

static mb_video_format luma_of(const struct mp_regular_imgfmt *reg, int width, int height)
{
    return (mb_video_format){
        .width = width,
        .height = height,
        .bit_depth = reg->component_size * 8 - (reg->component_pad < 0 ? -reg->component_pad : 0),
        .lsb_padding = reg->component_pad > 0 ? reg->component_pad : 0,
    };
}

bool mb_mpv_luma_format(int imgfmt, int width, int height, mb_video_format *format)
{
    struct mp_regular_imgfmt reg;
    if (!integer_samples(imgfmt, &reg)) {
        return false;
    }
    *format = luma_of(&reg, width, height);
    return true;
}

bool mb_mpv_video_format(int imgfmt, int width, int height, mb_video_format *format)
{
    struct mp_regular_imgfmt reg;
    bool interleaved;
    if (!integer_samples(imgfmt, &reg) || reg.chroma_xs > 1 || reg.chroma_ys > 1 ||
        !plain_planes(&reg, &interleaved)) {
        return false;
    }
    *format = luma_of(&reg, width, height);
    format->chroma_shift_x = reg.chroma_xs;
    format->chroma_shift_y = reg.chroma_ys;
    format->interleaved_chroma = interleaved;
    return true;
}

mb_host_image mb_mpv_host_image(struct mp_image *mpi)
{
    mb_host_image image = {{NULL}, {0}};
    for (int i = 0; i < 3 && i < mpi->num_planes; i++) {
        image.planes[i] = mpi->planes[i];
        image.strides[i] = mpi->stride[i];
    }
    return image;
}
