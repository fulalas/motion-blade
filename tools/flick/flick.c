#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FADE_FRAMES = 40, FLASH_PERIOD = 20, FLASH_PEAK = 10 };

typedef enum light_mode {
    LIGHT_FLICKER,
    LIGHT_FADE,
    LIGHT_FLASH,
} light_mode;

typedef struct clip {
    int width;
    int height;
    size_t luma;
    size_t frame;
} clip;

typedef struct tint {
    double y;
    double u;
    double v;
} tint;

static unsigned char clip_byte(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)lrint(v);
}

static bool parse_mode(const char *name, light_mode *mode)
{
    static const char *const NAMES[] = {"flicker", "fade", "flash"};
    for (int i = 0; i < 3; i++) {
        if (!strcmp(name, NAMES[i])) {
            *mode = (light_mode)i;
            return true;
        }
    }
    return false;
}

static bool read_header(FILE *in, FILE *out, clip *c)
{
    char header[512];
    if (!fgets(header, sizeof(header), in)) {
        return false;
    }
    fputs(header, out);
    char *w = strstr(header, " W");
    char *h = strstr(header, " H");
    char *colour = strstr(header, " C");
    c->width = w ? atoi(w + 2) : 0;
    c->height = h ? atoi(h + 2) : 0;
    bool bad = colour && (strncmp(colour + 2, "420", 3) || strstr(colour, "p1"));
    c->luma = (size_t)c->width * (size_t)c->height;
    c->frame = c->luma * 3 / 2;
    return c->width > 0 && c->height > 0 && c->width % 2 == 0 && c->height % 2 == 0 && !bad;
}

static void light_falloff(float *weights, const clip *c, light_mode mode)
{
    for (int y = 0; y < c->height; y++) {
        for (int x = 0; x < c->width; x++) {
            double dx = (x - 0.15 * c->width) / (0.55 * c->width);
            double dy = (y - 0.1 * c->height) / (0.55 * c->width);
            double e = exp(-(dx * dx + dy * dy));
            weights[y * c->width + x] = (float)(mode == LIGHT_FLASH ? 0.6 + 0.4 * e : 0.25 + 0.75 * e);
        }
    }
}

static void fade(unsigned char *frame, const clip *c, int n)
{
    double gain = fmax(1.0 - 0.8 * n / FADE_FRAMES, 0.2);
    for (size_t i = 0; i < c->luma; i++) {
        frame[i] = clip_byte(16 + (frame[i] - 16) * gain);
    }
    for (size_t i = c->luma; i < c->frame; i++) {
        frame[i] = clip_byte(128 + (frame[i] - 128) * gain);
    }
}

static double light_strength(light_mode mode, int n)
{
    if (mode == LIGHT_FLASH) {
        int m = n % FLASH_PERIOD;
        return m == FLASH_PEAK ? 1.0 : (m == FLASH_PEAK - 1 || m == FLASH_PEAK + 1) ? 0.5 : 0.0;
    }
    return n % 2 ? 0.5 : n % 4 == 0 ? 1.0 : 0.0;
}

typedef struct lighting {
    const float *weights;
    double strength;
    tint t;
} lighting;

static void light(unsigned char *frame, const clip *c, lighting l)
{
    for (size_t i = 0; i < c->luma; i++) {
        frame[i] = clip_byte(frame[i] + l.strength * l.t.y * l.weights[i]);
    }
    for (int y = 0; y < c->height / 2; y++) {
        for (int x = 0; x < c->width / 2; x++) {
            float w = l.weights[2 * y * c->width + 2 * x];
            unsigned char *u = frame + c->luma + y * (c->width / 2) + x;
            unsigned char *v = u + c->luma / 4;
            *u = clip_byte(*u + l.strength * l.t.u * w);
            *v = clip_byte(*v + l.strength * l.t.v * w);
        }
    }
}

static int process(FILE *in, FILE *out, light_mode mode)
{
    clip c;
    if (!read_header(in, out, &c)) {
        fprintf(stderr, "flick: needs 8-bit 4:2:0 y4m with even size\n");
        return 1;
    }
    unsigned char *frame = malloc(c.frame);
    float *weights = malloc(sizeof(float) * c.luma);
    if (!frame || !weights) {
        fprintf(stderr, "flick: out of memory\n");
        return 1;
    }
    light_falloff(weights, &c, mode);
    tint t = mode == LIGHT_FLASH ? (tint){50, 4, -2} : (tint){18, -6, 30};
    char line[256];
    for (int n = 0; fgets(line, sizeof(line), in) && fread(frame, 1, c.frame, in) == c.frame; n++) {
        if (mode == LIGHT_FADE) {
            fade(frame, &c, n);
        } else {
            light(frame, &c, (lighting){weights, light_strength(mode, n), t});
        }
        fputs("FRAME\n", out);
        fwrite(frame, 1, c.frame, out);
    }
    free(frame);
    free(weights);
    return 0;
}

int main(int argc, char **argv)
{
    light_mode mode = LIGHT_FLICKER;
    if (argc < 3 || (argc > 3 && !parse_mode(argv[3], &mode))) {
        fprintf(stderr, "flick in.y4m out.y4m [flicker|fade|flash]\n");
        return 1;
    }
    FILE *in = fopen(argv[1], "rb");
    FILE *out = fopen(argv[2], "wb");
    if (!in || !out) {
        fprintf(stderr, "flick: cannot open files\n");
        return 1;
    }
    int code = process(in, out, mode);
    fclose(in);
    fclose(out);
    return code;
}
