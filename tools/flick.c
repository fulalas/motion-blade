#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char clip(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)lrint(v);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "flick in.y4m out.y4m [flicker|fade|flash]\n");
        return 1;
    }
    const char *mode = argc > 3 ? argv[3] : "flicker";
    FILE *fi = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    char hdr[512], line[256];
    if (!fi || !fo || !fgets(hdr, sizeof hdr, fi))
        return 1;
    fputs(hdr, fo);
    char *pw = strstr(hdr, " W"), *ph = strstr(hdr, " H"), *pc = strstr(hdr, " C");
    int w = pw ? atoi(pw + 2) : 0, h = ph ? atoi(ph + 2) : 0;
    if (w <= 0 || h <= 0 || w % 2 || h % 2 || (pc && strncmp(pc + 2, "420", 3)) || (pc && strstr(pc, "p1"))) {
        fprintf(stderr, "flick: needs 8-bit 4:2:0 y4m with even size\n");
        return 1;
    }
    size_t ys = (size_t)w * h, fs = ys * 3 / 2;
    unsigned char *b = malloc(fs);
    float *a = malloc(sizeof(float) * ys);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double dx = (x - 0.15 * w) / (0.55 * w), dy = (y - 0.1 * h) / (0.55 * w);
            double e = exp(-(dx * dx + dy * dy));
            a[y * w + x] = strcmp(mode, "flash") ? 0.25 + 0.75 * e : 0.6 + 0.4 * e;
        }
    for (int n = 0; fgets(line, sizeof line, fi) && fread(b, 1, fs, fi) == fs; n++) {
        if (!strcmp(mode, "fade")) {
            double g = 1.0 - 0.8 * n / 40.0;
            if (g < 0.2)
                g = 0.2;
            for (size_t i = 0; i < ys; i++)
                b[i] = clip(16 + (b[i] - 16) * g);
            for (size_t i = ys; i < fs; i++)
                b[i] = clip(128 + (b[i] - 128) * g);
        } else {
            int flash = !strcmp(mode, "flash");
            double ky = flash ? 50 : 18, ku = flash ? 4 : -6, kv = flash ? -2 : 30;
            int m = n % 20;
            double g = flash ? (m == 10 ? 1.0 : m == 9 || m == 11 ? 0.5 : 0.0) : n % 2 ? 0.5 : n % 4 == 0 ? 1.0 : 0.0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    b[y * w + x] = clip(b[y * w + x] + g * ky * a[y * w + x]);
            for (int y = 0; y < h / 2; y++)
                for (int x = 0; x < w / 2; x++) {
                    float aa = a[2 * y * w + 2 * x];
                    unsigned char *u = b + ys + y * (w / 2) + x, *v = u + ys / 4;
                    *u = clip(*u + g * ku * aa);
                    *v = clip(*v + g * kv * aa);
                }
        }
        fputs("FRAME\n", fo);
        fwrite(b, 1, fs, fo);
    }
    fclose(fo);
    return 0;
}
