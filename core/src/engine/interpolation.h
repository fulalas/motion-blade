#ifndef MB_INTERPOLATION_H
#define MB_INTERPOLATION_H

#include "engine/engine_state.h"

typedef struct pair_choice {
    int a;
    int b;
    int c;
    int z;
    const pair_motion *current;
    const pair_motion *next;
    const pair_motion *past;
} pair_choice;

pair_choice choose_pair(const mb_engine *engine, int end);
void record_interpolation(mb_engine *engine, const pair_choice *pair, double t);

#endif
