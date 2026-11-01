#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void options_usage(void)
{
    fprintf(stderr, "eval in.y4m|in.mkv [-n frames] [-skip k] [-step s] [-mode motionblade|blend|repeat]\n"
                    "     [-cmp full.y4m] [-o out.y4m] [-p name=value]... [-q] [-nometric] [-repeat r]\n"
                    "     [-backend vulkan|cpu] [-dump frame[:fields.ppm]] [-fmt ");
    test_formats_list();
    fprintf(stderr, "]\n");
}

static bool parse_mode(const char *name, eval_mode *mode)
{
    if (!strcmp(name, "motionblade")) {
        *mode = MODE_INTERPOLATE;
    } else if (!strcmp(name, "blend")) {
        *mode = MODE_BLEND;
    } else if (!strcmp(name, "repeat")) {
        *mode = MODE_REPEAT;
    } else {
        return false;
    }
    return true;
}

static void parse_dump(char *value, eval_options *o)
{
    o->dump_frame = atoi(value);
    char *colon = strchr(value, ':');
    o->dump_path = colon ? colon + 1 : NULL;
}

static bool parse_valued(const char *flag, char *value, eval_options *o)
{
    if (!strcmp(flag, "-n")) {
        o->max_frames = atoi(value);
    } else if (!strcmp(flag, "-skip")) {
        o->skip = atoi(value);
    } else if (!strcmp(flag, "-step")) {
        o->step = atoi(value);
    } else if (!strcmp(flag, "-repeat")) {
        o->repeat = atoi(value);
    } else if (!strcmp(flag, "-mode")) {
        return parse_mode(value, &o->mode);
    } else if (!strcmp(flag, "-cmp")) {
        o->compare = value;
    } else if (!strcmp(flag, "-o")) {
        o->output = value;
    } else if (!strcmp(flag, "-backend")) {
        o->backend = value;
    } else if (!strcmp(flag, "-dump")) {
        parse_dump(value, o);
    } else if (!strcmp(flag, "-fmt")) {
        o->format = test_format_find(value);
        return o->format != NULL;
    } else if (!strcmp(flag, "-p") && o->param_count < MAX_PARAMS) {
        o->params[o->param_count++] = value;
    } else {
        return false;
    }
    return true;
}

static bool parse_switch(const char *flag, eval_options *o)
{
    if (!strcmp(flag, "-q")) {
        o->quiet = true;
    } else if (!strcmp(flag, "-nometric")) {
        o->no_metrics = true;
        o->quiet = true;
    } else {
        return false;
    }
    return true;
}

static eval_options defaults(void)
{
    return (eval_options){
        .backend = "vulkan",
        .max_frames = 1000,
        .step = 2,
        .repeat = 1,
        .dump_frame = -1,
        .mode = MODE_INTERPOLATE,
        .format = test_format_default(),
    };
}

bool options_parse(int argc, char **argv, eval_options *o)
{
    *o = defaults();
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (arg[0] != '-') {
            o->input = arg;
            continue;
        }
        if (parse_switch(arg, o)) {
            continue;
        }
        if (i + 1 >= argc || !parse_valued(arg, argv[i + 1], o)) {
            return false;
        }
        i++;
    }
    return o->input && o->step >= 1;
}
