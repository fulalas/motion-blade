#ifndef MB_INTERPOLATION_H
#define MB_INTERPOLATION_H

#include "engine/engine_state.h"

typedef struct pair_choice {
    int a;
    int b;
    int c;
    int z;
    mb_buffer *forward;
    mb_buffer *backward;
    mb_buffer *next_forward;
    mb_buffer *previous_backward;
    mb_buffer *current_stats;
    mb_buffer *previous_stats;
    bool lookahead;
    bool past;
} pair_choice;

pair_choice choose_pair(const mb_engine *engine, bool lookahead);
void record_interpolation(mb_engine *engine, const pair_choice *pair, double t);

#endif
