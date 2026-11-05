#pragma once

#include <stdbool.h>

#include "motionblade/video.h"

struct mp_image;

bool mb_mpv_video_format(int imgfmt, int width, int height, mb_video_format *format);
bool mb_mpv_luma_format(int imgfmt, int width, int height, mb_video_format *format);
mb_host_image mb_mpv_host_image(struct mp_image *mpi);
