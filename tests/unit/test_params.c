#include "check.h"
#include "motionblade/params.h"

static bool defaults_match_tuning(void)
{
    mb_params p = mb_params_default();
    CHECK(p.search.full_search_radius == 16);
    CHECK(p.blend.softness == 250.0f);
    CHECK(p.framing.settle_frames == 48);
    return true;
}

static bool sets_known_values(void)
{
    mb_params p = mb_params_default();
    CHECK(mb_params_set(&p, "penalty_zero", 3.5) == MB_SUCCESS);
    CHECK(p.search.penalty_zero == 3.5f);
    CHECK(mb_params_set(&p, "iterations", 5) == MB_SUCCESS);
    CHECK(p.search.iterations == 5);
    return true;
}

static bool rejects_unknown_names(void)
{
    mb_params p = mb_params_default();
    CHECK(mb_params_set(&p, "no_such_param", 1) == MB_ERROR_INVALID);
    return true;
}

static bool rejects_out_of_range(void)
{
    mb_params p = mb_params_default();
    CHECK(mb_params_set(&p, "full_search_radius", 17) == MB_ERROR_INVALID);
    CHECK(mb_params_set(&p, "base_iterations", 0) == MB_ERROR_INVALID);
    CHECK(p.search.full_search_radius == 16);
    return true;
}

static bool keeps_mean_removal_ordered(void)
{
    mb_params p = mb_params_default();
    CHECK(mb_params_set(&p, "mean_removal_lo", 1.0) == MB_ERROR_INVALID);
    CHECK(mb_params_set(&p, "mean_removal_hi", 0.4) == MB_ERROR_INVALID);
    CHECK(p.search.mean_removal_lo == 0.5f);
    return true;
}

int main(void)
{
    static const test_case tests[] = {
        {"defaults match tuning", defaults_match_tuning},
        {"sets known values", sets_known_values},
        {"rejects unknown names", rejects_unknown_names},
        {"rejects out of range", rejects_out_of_range},
        {"keeps mean removal ordered", keeps_mean_removal_ordered},
    };
    return RUN_ALL(tests);
}
