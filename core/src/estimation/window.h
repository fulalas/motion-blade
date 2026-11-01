#ifndef MB_WINDOW_H
#define MB_WINDOW_H

#include "common/texture.h"
#include "common/vec.h"
#include "motionblade/layout.h"

typedef struct window {
    float rows[MB_WINDOW_SIZE][MB_WINDOW_SIZE];
} window;

typedef struct sad_result {
    float sad;
    float diff_sum;
} sad_result;

typedef struct sad_pair {
    float plain;
    float zero_mean;
} sad_pair;

void window_load_texels(window *w, const ref_texture *texture, ivec2 origin);
void window_load_exact(window *w, const ref_texture *texture, ivec2 origin, vec2 offset);
void window_load_hardware(window *w, const ref_texture *texture, ivec2 origin, vec2 offset);
sad_result window_zsad(const window *a, const window *b, float mean_weight);
sad_pair window_sad_pair(const window *a, const window *b);
float window_sum(const window *w);
float window_texture(const window *w, const window *below);
float cluster_sum(const float lanes[MB_WINDOW_SIZE]);
float block_dc(float diff_sum);
float mean_weight(float lo, float hi, float min_abs_dc);

#endif
