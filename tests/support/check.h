#ifndef MB_TEST_CHECK_H
#define MB_TEST_CHECK_H

#include <stdbool.h>
#include <stdio.h>

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
            return false;                                                                  \
        }                                                                                  \
    } while (0)

typedef struct test_case {
    const char *name;
    bool (*run)(void);
} test_case;

int run_tests(const test_case *tests, int count);

#define RUN_ALL(tests) run_tests(tests, (int)(sizeof(tests) / sizeof(tests[0])))

#endif
