#include "check.h"

int run_tests(const test_case *tests, int count)
{
    int failed = 0;
    for (int i = 0; i < count; i++) {
        bool ok = tests[i].run();
        printf("%s %s\n", ok ? "ok  " : "FAIL", tests[i].name);
        failed += !ok;
    }
    printf("%d/%d passed\n", count - failed, count);
    return failed ? 1 : 0;
}
