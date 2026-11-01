#ifndef EVAL_SEQUENCE_H
#define EVAL_SEQUENCE_H

#include <stddef.h>
#include <stdint.h>

typedef struct sequence {
    int width;
    int height;
    int count;
    uint8_t **frames;
    char header[256];
} sequence;

typedef struct sequence_request {
    const char *path;
    int max_frames;
    int skip;
} sequence_request;

int sequence_read(const sequence_request *request, sequence *s);
void sequence_free(sequence *s);
size_t frame_bytes(int width, int height);

#endif
