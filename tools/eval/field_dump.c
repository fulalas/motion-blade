#include "field_dump.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const float PI = 3.14159265f;
static const float VECTOR_SCALE = 40.0f;

typedef struct rgb {
    float r;
    float g;
    float b;
} rgb;

static rgb hue_color(float hue)
{
    int sector = (int)hue % 6;
    float f = hue - floorf(hue);
    switch (sector) {
    case 0:
        return (rgb){1, f, 0};
    case 1:
        return (rgb){1 - f, 1, 0};
    case 2:
        return (rgb){0, 1, f};
    case 3:
        return (rgb){0, 1 - f, 1};
    case 4:
        return (rgb){f, 0, 1};
    default:
        return (rgb){1, 0, 1 - f};
    }
}

static void put_vector(FILE *file, float x, float y)
{
    float strength = fminf(1.0f, sqrtf(x * x + y * y) / VECTOR_SCALE);
    rgb c = hue_color((atan2f(y, x) + PI) / (2 * PI) * 6.0f);
    unsigned char pixel[3] = {(unsigned char)(c.r * strength * 255), (unsigned char)(c.g * strength * 255),
                              (unsigned char)(c.b * strength * 255)};
    fwrite(pixel, 1, 3, file);
}

void dump_fields(mb_engine *engine, const char *path, int width, int height)
{
    FILE *file = fopen(path, "wb");
    float *vectors = malloc(sizeof(float) * 4 * (size_t)((width + 7) / 8) * (size_t)((height + 7) / 8));
    if (!file || !vectors) {
        fprintf(stderr, "cannot write %s\n", path);
        free(vectors);
        if (file) {
            fclose(file);
        }
        return;
    }
    mb_field_kind kinds[3] = {MB_FIELD_FORWARD, MB_FIELD_BACKWARD, MB_FIELD_SELECTION};
    for (int k = 0; k < 3; k++) {
        mb_field_dump dump = {vectors, 0, 0};
        if (mb_engine_read_field(engine, kinds[k], &dump) != MB_SUCCESS) {
            break;
        }
        if (k == 0) {
            fprintf(file, "P6 %d %d 255\n", dump.blocks_x, dump.blocks_y * 3);
        }
        for (int i = 0; i < dump.blocks_x * dump.blocks_y; i++) {
            put_vector(file, vectors[4 * i], vectors[4 * i + 1]);
        }
    }
    fclose(file);
    free(vectors);
}
