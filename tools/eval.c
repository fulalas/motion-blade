#define _GNU_SOURCE
#include "memc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int w, h, n;
    uint8_t **f;
    char hdr[256];
} seq;

static int read_y4m(const char *path, seq *s, int maxn, int skip)
{
    size_t len = strlen(path);
    int piped = !(len > 4 && !strcmp(path + len - 4, ".y4m")) && strncmp(path, "/dev/", 5);
    FILE *fp;
    if (piped) {
        char cmd[4200];
        snprintf(cmd, sizeof(cmd), "ffmpeg -nostdin -v fatal -i '%s' -frames:v %d -pix_fmt yuv420p -f yuv4mpegpipe -",
                 path, maxn + skip);
        fp = popen(cmd, "r");
    } else {
        fp = fopen(path, "rb");
    }
    if (!fp)
        return -1;
    if (!fgets(s->hdr, sizeof(s->hdr), fp)) {
        piped ? pclose(fp) : fclose(fp);
        return -1;
    }
    char *p = strstr(s->hdr, " W");
    s->w = atoi(p + 2);
    p = strstr(s->hdr, " H");
    s->h = atoi(p + 2);
    size_t fsz = (size_t)s->w * s->h * 3 / 2;
    s->f = calloc(maxn, sizeof(*s->f));
    s->n = 0;
    char line[256];
    int idx = 0;
    while (s->n < maxn && fgets(line, sizeof(line), fp)) {
        uint8_t *buf = malloc(fsz);
        if (fread(buf, 1, fsz, fp) != fsz) {
            free(buf);
            break;
        }
        if (idx++ < skip) {
            free(buf);
            continue;
        }
        s->f[s->n++] = buf;
    }
    piped ? pclose(fp) : fclose(fp);
    return 0;
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
    double se = 0;
    for (size_t i = 0; i < n; i++) {
        int d = a[i] - b[i];
        se += d * d;
    }
    double mse = se / n;
    return mse < 1e-10 ? 99 : 10 * log10(255.0 * 255.0 / mse);
}

static int cmpdbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static double worst_blocks(const uint8_t *a, const uint8_t *b, int w, int h)
{
    int bw = w / 16, bh = h / 16, n = bw * bh;
    double *m = malloc(sizeof(double) * n);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            double se = 0;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    int d = a[(by * 16 + y) * w + bx * 16 + x] - b[(by * 16 + y) * w + bx * 16 + x];
                    se += d * d;
                }
            m[by * bw + bx] = se / 256;
        }
    qsort(m, n, sizeof(double), cmpdbl);
    int k = n / 100 > 0 ? n / 100 : 1;
    double s = 0;
    for (int i = 0; i < k; i++)
        s += m[i];
    free(m);
    s /= k;
    return s < 1e-10 ? 99 : 10 * log10(255.0 * 255.0 / s);
}

static double ssim(const uint8_t *a, const uint8_t *b, int w, int h)
{
    const double C1 = 6.5025, C2 = 58.5225;
    double sum = 0;
    int cnt = 0;
    for (int y = 0; y + 8 <= h; y += 4)
        for (int x = 0; x + 8 <= w; x += 4) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) {
                    int pa = a[(y + j) * w + x + i], pb = b[(y + j) * w + x + i];
                    sa += pa;
                    sb += pb;
                    saa += pa * pa;
                    sbb += pb * pb;
                    sab += pa * pb;
                }
            double ma = sa / 64, mb = sb / 64;
            double va = saa / 64 - ma * ma, vb = sbb / 64 - mb * mb, cab = sab / 64 - ma * mb;
            sum += ((2 * ma * mb + C1) * (2 * cab + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
            cnt++;
        }
    return sum / cnt;
}

typedef struct {
    uint8_t *buf;
    memc_image im;
} conv;

static void conv_alloc(conv *cv, int fm, int w, int h)
{
    int bps = fm >= 2 ? 2 : 1;
    size_t ys = (size_t)w * h * bps, cs = (size_t)(w / 2) * (h / 2) * bps;
    cv->buf = malloc(ys + 2 * cs);
    cv->im.data[0] = cv->buf;
    cv->im.stride[0] = (ptrdiff_t)w * bps;
    if (fm == 1 || fm == 2) {
        cv->im.data[1] = cv->buf + ys;
        cv->im.stride[1] = (ptrdiff_t)w * bps;
    } else {
        cv->im.data[1] = cv->buf + ys;
        cv->im.data[2] = cv->buf + ys + cs;
        cv->im.stride[1] = cv->im.stride[2] = (ptrdiff_t)(w / 2) * bps;
    }
}

static void to_fmt(conv *cv, int fm, const uint8_t *src, int w, int h)
{
    const uint8_t *Y = src, *U = src + (size_t)w * h, *V = U + (size_t)(w / 2) * (h / 2);
    int sh = fm == 2 ? 8 : 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int v = Y[y * w + x];
            if (fm < 2)
                cv->im.data[0][y * cv->im.stride[0] + x] = v;
            else
                ((uint16_t *)(cv->im.data[0] + y * cv->im.stride[0]))[x] = (uint16_t)(fm == 2 ? v * 257 : v << sh);
        }
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            int u = U[y * (w / 2) + x], v = V[y * (w / 2) + x];
            if (fm == 1) {
                cv->im.data[1][y * cv->im.stride[1] + 2 * x] = u;
                cv->im.data[1][y * cv->im.stride[1] + 2 * x + 1] = v;
            } else if (fm == 2) {
                uint16_t *r = (uint16_t *)(cv->im.data[1] + y * cv->im.stride[1]);
                r[2 * x] = (uint16_t)(u * 257);
                r[2 * x + 1] = (uint16_t)(v * 257);
            } else if (fm == 3) {
                ((uint16_t *)(cv->im.data[1] + y * cv->im.stride[1]))[x] = (uint16_t)(u << 2);
                ((uint16_t *)(cv->im.data[2] + y * cv->im.stride[2]))[x] = (uint16_t)(v << 2);
            }
        }
}

static int rd16(const uint8_t *p, int fm)
{
    int v = *(const uint16_t *)p;
    return fm == 2 ? (v + 128) / 257 : (v + 2) >> 2;
}

static void from_fmt(const conv *cv, int fm, uint8_t *dst, int w, int h)
{
    uint8_t *Y = dst, *U = dst + (size_t)w * h, *V = U + (size_t)(w / 2) * (h / 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            Y[y * w + x] = fm < 2 ? cv->im.data[0][y * cv->im.stride[0] + x]
                                  : (uint8_t)rd16(cv->im.data[0] + y * cv->im.stride[0] + 2 * x, fm);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            const uint8_t *r1 = cv->im.data[1] + y * cv->im.stride[1];
            if (fm == 1) {
                U[y * (w / 2) + x] = r1[2 * x];
                V[y * (w / 2) + x] = r1[2 * x + 1];
            } else if (fm == 2) {
                U[y * (w / 2) + x] = (uint8_t)rd16(r1 + 4 * x, fm);
                V[y * (w / 2) + x] = (uint8_t)rd16(r1 + 4 * x + 2, fm);
            } else {
                const uint8_t *r2 = cv->im.data[2] + y * cv->im.stride[2];
                U[y * (w / 2) + x] = (uint8_t)rd16(r1 + 2 * x, fm);
                V[y * (w / 2) + x] = (uint8_t)rd16(r2 + 2 * x, fm);
            }
        }
}

static void put_vec(FILE *fp, float x, float y, float scale)
{
    float mag = sqrtf(x * x + y * y), ang = atan2f(y, x);
    float v = fminf(1.0f, mag / scale);
    float hh = (ang + 3.14159265f) / (2 * 3.14159265f) * 6.0f;
    int k = (int)hh % 6;
    float fr = hh - floorf(hh);
    float r = 0, g = 0, b = 0;
    switch (k) {
    case 0: r = 1; g = fr; break;
    case 1: r = 1 - fr; g = 1; break;
    case 2: g = 1; b = fr; break;
    case 3: g = 1 - fr; b = 1; break;
    case 4: r = fr; b = 1; break;
    default: r = 1; b = 1 - fr; break;
    }
    unsigned char px[3] = {(unsigned char)(r * v * 255), (unsigned char)(g * v * 255), (unsigned char)(b * v * 255)};
    fwrite(px, 1, 3, fp);
}

static void dump_fields(memc_ctx *c, const char *path)
{
    float *f = malloc(sizeof(float) * 4 * 512 * 512);
    int w, h;
    FILE *fp = fopen(path, "wb");
    for (int which = 0; which < 3; which++) {
        if (memc_debug_field(c, which, f, &w, &h) < 0)
            break;
        if (which == 0)
            fprintf(fp, "P6 %d %d 255\n", w, h * 3);
        for (int i = 0; i < w * h; i++)
            put_vec(fp, f[4 * i], f[4 * i + 1], 40.0f);
    }
    fclose(fp);
    free(f);
}

static void usage(void)
{
    fprintf(stderr, "eval in.y4m|in.mkv [-n frames] [-skip k] [-step s] [-mode memc|blend|repeat] [-cmp full.y4m]\n"
                    "     [-o out.y4m] [-p name=value]... [-threads n] [-q]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *cmp = NULL, *mode = "memc";
    int maxn = 1000, step = 2, threads = 0, quiet = 0, skip = 0, nometric = 0, repeat = 1, fm = 0, dumpk = -1;
    char *params[64];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) maxn = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-skip") && i + 1 < argc) skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-step") && i + 1 < argc) step = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "-cmp") && i + 1 < argc) cmp = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) params[np++] = argv[++i];
        else if (!strcmp(argv[i], "-threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc) dumpk = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-fmt") && i + 1 < argc) {
            i++;
            fm = !strcmp(argv[i], "nv12") ? 1 : !strcmp(argv[i], "p010") ? 2 : !strcmp(argv[i], "yuv420p10") ? 3 : 0;
        }
        else if (!strcmp(argv[i], "-repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-nometric")) nometric = 1, quiet = 1;
        else if (argv[i][0] != '-') in = argv[i];
        else usage();
    }
    if (!in)
        usage();
    seq s, sc = {0};
    if (read_y4m(in, &s, maxn, skip) < 0) {
        fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    if (cmp && read_y4m(cmp, &sc, maxn, skip) < 0) {
        fprintf(stderr, "cannot read %s\n", cmp);
        return 1;
    }
    int w = s.w, h = s.h;
    size_t ysz = (size_t)w * h, fsz = ysz * 3 / 2;
    memc_format fmt = {w, h, fm == 2 ? 16 : fm == 3 ? 10 : 8, 1, 1, fm == 1 || fm == 2};
    conv ca, cb, co;
    if (fm) {
        conv_alloc(&ca, fm, w, h);
        conv_alloc(&cb, fm, w, h);
        conv_alloc(&co, fm, w, h);
    }
    memc_ctx *c = memc_create(&fmt, threads);
    for (int i = 0; i < np; i++) {
        char *eq = strchr(params[i], '=');
        if (!eq) usage();
        *eq = 0;
        if (memc_set_param(c, params[i], atof(eq + 1)) < 0) {
            fprintf(stderr, "unknown param %s\n", params[i]);
            return 1;
        }
    }
    FILE *fo = out ? fopen(out, "wb") : NULL;
    if (fo)
        fputs(s.hdr, fo);
    uint8_t *res = malloc(fsz);
    double swb = 0, spc = 0, sp = 0, ss = 0, sme = 0, sit = 0, worst = 99;
    int cnt = 0, worsti = -1, la = 1;
    for (int i = 0; i < np; i++)
        if (!strcmp(params[i], "la"))
            la = atoi(params[i] + 3);
    double wall0 = 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        wall0 = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
    }
    for (int rep_i = 0; rep_i < repeat; rep_i++)
    for (int k = 0; (k + 1) * step < s.n; k++) {
        if (k == 0 && rep_i > 0)
            memc_reset(c);
        uint8_t *A = s.f[k * step], *B = s.f[(k + 1) * step];
        if (!strcmp(mode, "memc")) {
            for (int j = k == 0 ? 0 : k + 1 + la; j <= k + 1 + la; j++) {
                if (j * step >= s.n) {
                    memc_flush(c);
                    continue;
                }
                uint8_t *P = s.f[j * step];
                if (fm) {
                    to_fmt(&cb, fm, P, w, h);
                    memc_push(c, &cb.im);
                } else {
                    memc_image ip = {{P, P + ysz, P + ysz + ysz / 4}, {w, w / 2, w / 2}};
                    memc_push(c, &ip);
                }
            }
        }
        if (fo)
            fprintf(fo, "FRAME\n"), fwrite(A, 1, fsz, fo);
        for (int m = 1; m < step; m++) {
            double t = (double)m / step;
            const uint8_t *R = res;
            memc_stats st = {0};
            if (cmp) {
                R = sc.f[k * step + m];
            } else if (!strcmp(mode, "memc")) {
                memc_image io = {{res, res + ysz, res + ysz + ysz / 4}, {w, w / 2, w / 2}};
                if (fm) {
                    memc_interpolate(c, t, &co.im);
                    from_fmt(&co, fm, res, w, h);
                } else {
                    memc_interpolate(c, t, &io);
                }
                memc_get_stats(c, &st);
            } else if (!strcmp(mode, "blend")) {
                for (size_t i = 0; i < fsz; i++)
                    res[i] = (uint8_t)lrint(A[i] * (1 - t) + B[i] * t);
            } else {
                memcpy(res, t < 0.5 ? A : B, fsz);
            }
            if (k * step + m == dumpk && !strcmp(mode, "memc")) {
                dump_fields(c, "/tmp/memc-out/fields.ppm");
            }
            const uint8_t *T = s.f[k * step + m];
            double p = nometric ? 0 : psnr(R, T, ysz), q = nometric ? 0 : ssim(R, T, w, h);
            double wb = nometric ? 0 : worst_blocks(R, T, w, h);
            if (!quiet)
                printf("frame %3d  local %6.2f  psnr %6.2f  ssim %.4f  me %6.2fms  interp %6.2fms  err %.1f%s\n",
                       k * step + m, wb, p, q, st.me_ms, st.interp_ms, st.mean_err, st.scene_cut ? " CUT" : "");
            sp += p;
            spc += nometric ? 0 : psnr(R + ysz, T + ysz, ysz / 2);
            swb += wb;
            ss += q;
            sme += st.me_ms;
            sit += st.interp_ms;
            if (p < worst) {
                worst = p;
                worsti = k * step + m;
            }
            cnt++;
            if (fo)
                fprintf(fo, "FRAME\n"), fwrite(R, 1, fsz, fo);
        }
    }
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        if (nometric)
            printf("wall %.2f ms per interpolated frame\n", (ts.tv_sec * 1e3 + ts.tv_nsec / 1e6 - wall0) / cnt);
    }
    printf("MEAN local %.3f | chroma psnr %.3f | ", swb / cnt, spc / cnt);
    printf("MEAN psnr %.3f ssim %.4f  worst %.2f@%d  me %.2fms interp %.2fms  (%d frames)\n", sp / cnt, ss / cnt,
           worst, worsti, sme / cnt, sit / cnt, cnt);
    if (fo)
        fclose(fo);
    memc_destroy(c);
    return 0;
}
