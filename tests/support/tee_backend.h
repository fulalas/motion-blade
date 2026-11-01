#ifndef MB_TEST_TEE_BACKEND_H
#define MB_TEST_TEE_BACKEND_H

#include "motionblade/backend.h"

enum { TEE_MAX_PASSES = 32 };

typedef struct tee_pass_report {
    const char *name;
    int runs;
    double worst_agreement;
    double worst_difference;
} tee_pass_report;

typedef struct tee_report {
    tee_pass_report passes[TEE_MAX_PASSES];
    int count;
} tee_report;

mb_status tee_backend_create(mb_backend reference, mb_backend device, tee_report *report, mb_backend *tee);
const tee_pass_report *tee_find(const tee_report *report, const char *name);

#endif
