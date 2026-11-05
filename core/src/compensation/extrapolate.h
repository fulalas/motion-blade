#ifndef MB_EXTRAPOLATE_H
#define MB_EXTRAPOLATE_H

#include "motionblade/passes.h"

typedef struct cut_plan {
    bool moves;
    bool from_b;
} cut_plan;

cut_plan cut_plan_of(const mb_interpolate_pass *pass);
void ref_extrapolate(const mb_interpolate_pass *pass, bool from_b);

#endif
