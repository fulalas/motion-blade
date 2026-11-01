#ifndef MB_CUBIC_H
#define MB_CUBIC_H

#include "common/texture.h"
#include "common/vec.h"

float cubic_sample(const ref_texture *texture, vec2 position, int channel);

#endif
