#ifndef EVAL_OPTIONS_H
#define EVAL_OPTIONS_H

#include <stdbool.h>

#include "pixel_formats.h"

enum { MAX_PARAMS = 64 };

typedef enum eval_mode {
    MODE_INTERPOLATE,
    MODE_BLEND,
    MODE_REPEAT,
} eval_mode;

typedef struct eval_options {
    const char *input;
    const char *output;
    const char *compare;
    const char *dump_path;
    const char *backend;
    int max_frames;
    int skip;
    int step;
    int repeat;
    int dump_frame;
    bool quiet;
    bool no_metrics;
    eval_mode mode;
    const test_format *format;
    char *params[MAX_PARAMS];
    int param_count;
} eval_options;

bool options_parse(int argc, char **argv, eval_options *options);
void options_usage(void);

#endif
