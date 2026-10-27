#define _GNU_SOURCE
#include "motionblade.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int w, h, n;
    uint8_t **f;
    char hdr[256];
} seq;

static void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory (%zu bytes)\n", n);
        exit(1);
    }
    return p;
}

static size_t frame_size(int w, int h)
{
    return (size_t)w * h * 3 / 2;
}

static mb_image yuv420(uint8_t *p, int w, int h)
{
    size_t ys = (size_t)w * h;
    return (mb_image){{p, p + ys, p + ys + ys / 4}, {w, w / 2, w / 2}};
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static FILE *open_ffmpeg(const char *path, int nframes, pid_t *pid)
{
    char nf[16];
    snprintf(nf, sizeof(nf), "%d", nframes);
    char *const argv[] = {"ffmpeg", "-nostdin", "-v", "fatal", "-i", (char *)path, "-frames:v", nf,
                          "-pix_fmt", "yuv420p", "-f", "yuv4mpegpipe", "-", NULL};
    int fd[2];
    if (pipe(fd) < 0)
        return NULL;
    *pid = fork();
    if (*pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return NULL;
    }
    if (*pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    return fdopen(fd[0], "r");
}

static void close_in(FILE *fp, pid_t pid)
{
    fclose(fp);
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static int read_y4m(const char *path, seq *s, int maxn, int skip)
{
    size_t len = strlen(path);
    int piped = !(len > 4 && !strcmp(path + len - 4, ".y4m")) && strncmp(path, "/dev/", 5);
    pid_t pid = -1;
    FILE *fp = piped ? open_ffmpeg(path, maxn + skip, &pid) : fopen(path, "rb");
    if (!fp)
        return -1;
    if (!fgets(s->hdr, sizeof(s->hdr), fp)) {
        close_in(fp, pid);
        return -1;
    }
    char *pw = strstr(s->hdr, " W"), *ph = strstr(s->hdr, " H"), *pc = strstr(s->hdr, " C");
    s->w = pw ? atoi(pw + 2) : 0;
    s->h = ph ? atoi(ph + 2) : 0;
    if (s->w <= 0 || s->h <= 0 || s->w % 2 || s->h % 2 || (pc && (strncmp(pc + 2, "420", 3) || strstr(pc, "p1")))) {
        fprintf(stderr, "%s: needs 8-bit 4:2:0 with even width and height\n", path);
        close_in(fp, pid);
        return -1;
    }
    size_t fsz = frame_size(s->w, s->h);
    s->f = xalloc((size_t)(maxn > 0 ? maxn : 1) * sizeof(*s->f));
    s->n = 0;
    char line[256];
    int idx = 0;
    while (s->n < maxn && fgets(line, sizeof(line), fp)) {
        uint8_t *buf = xalloc(fsz);
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
    close_in(fp, pid);
    return 0;
}

static double db(double mse)
{
    return mse < 1e-10 ? 99 : 10 * log10(255.0 * 255.0 / mse);
}

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
    double se = 0;
    for (size_t i = 0; i < n; i++) {
        int d = a[i] - b[i];
        se += d * d;
    }
    return db(se / n);
}

static int cmpdbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static double worst_blocks(const uint8_t *a, const uint8_t *b, int w, int h)
{
    int bw = w / 16, bh = h / 16, n = bw * bh;
    double *m = xalloc(sizeof(double) * (size_t)n);
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
    return db(s / k);
}

static double ssim(const uint8_t *a, const uint8_t *b, int w, int h)
{
    const double C1 = 6.5025, C2 = 58.5225;
    size_t pw = (size_t)w + 1;
    int64_t *P = xalloc(sizeof(int64_t) * 5 * pw);
    int64_t *pa = P, *pb = pa + pw, *paa = pb + pw, *pbb = paa + pw, *pab = pbb + pw;
    double sum = 0;
    int cnt = 0;
    for (int y = 0; y + 8 <= h; y += 4) {
        memset(P, 0, sizeof(int64_t) * 5 * pw);
        for (int j = 0; j < 8; j++) {
            const uint8_t *ra = a + (size_t)(y + j) * w, *rb = b + (size_t)(y + j) * w;
            for (int x = 0; x < w; x++) {
                int va = ra[x], vb = rb[x];
                pa[x + 1] += va;
                pb[x + 1] += vb;
                paa[x + 1] += va * va;
                pbb[x + 1] += vb * vb;
                pab[x + 1] += va * vb;
            }
        }
        for (int x = 0; x < w; x++) {
            pa[x + 1] += pa[x];
            pb[x + 1] += pb[x];
            paa[x + 1] += paa[x];
            pbb[x + 1] += pbb[x];
            pab[x + 1] += pab[x];
        }
        for (int x = 0; x + 8 <= w; x += 4) {
            double sa = pa[x + 8] - pa[x], sb = pb[x + 8] - pb[x];
            double saa = paa[x + 8] - paa[x], sbb = pbb[x + 8] - pbb[x], sab = pab[x + 8] - pab[x];
            double ma = sa / 64, mb = sb / 64;
            double va = saa / 64 - ma * ma, vb = sbb / 64 - mb * mb, cab = sab / 64 - ma * mb;
            sum += ((2 * ma * mb + C1) * (2 * cab + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
            cnt++;
        }
    }
    free(P);
    return sum / cnt;
}

typedef struct {
    const char *name;
    int depth, nv, lsb_pad, bps, sh;
} tfmt;

static const tfmt fmts[] = {
    {"yuv420p", 8, 0, 0, 1, 0},
    {"nv12", 8, 1, 0, 1, 0},
    {"p010", 16, 1, 6, 2, 8},
    {"yuv420p10", 10, 0, 0, 2, 2},
    {"p016", 16, 1, 0, 2, 0},
};

typedef struct {
    uint8_t *buf;
    mb_image im;
} conv;

static void conv_alloc(conv *cv, const tfmt *f, int w, int h)
{
    int bps = f->bps;
    size_t ys = (size_t)w * h * bps, cs = (size_t)(w / 2) * (h / 2) * bps;
    cv->buf = xalloc(ys + 2 * cs);
    cv->im.data[0] = cv->buf;
    cv->im.stride[0] = (ptrdiff_t)w * bps;
    cv->im.data[1] = cv->buf + ys;
    if (f->nv) {
        cv->im.stride[1] = (ptrdiff_t)w * bps;
    } else {
        cv->im.data[2] = cv->buf + ys + cs;
        cv->im.stride[1] = cv->im.stride[2] = (ptrdiff_t)(w / 2) * bps;
    }
}

static void wr(uint8_t *p, int v, const tfmt *f)
{
    if (f->bps == 1)
        *p = (uint8_t)v;
    else
        *(uint16_t *)p = (uint16_t)(f->sh ? v << f->sh : v * 257);
}

static int rd(const uint8_t *p, const tfmt *f)
{
    if (f->bps == 1)
        return *p;
    int v = *(const uint16_t *)p;
    v = f->sh ? (v + (1 << (f->sh - 1))) >> f->sh : (v + 128) / 257;
    return v > 255 ? 255 : v;
}

static void to_fmt(conv *cv, const tfmt *f, const uint8_t *src, int w, int h)
{
    const uint8_t *Y = src, *U = src + (size_t)w * h, *V = U + (size_t)(w / 2) * (h / 2);
    int bps = f->bps;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            wr(cv->im.data[0] + y * cv->im.stride[0] + x * bps, Y[y * w + x], f);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            int u = U[y * (w / 2) + x], v = V[y * (w / 2) + x];
            uint8_t *r1 = cv->im.data[1] + y * cv->im.stride[1];
            if (f->nv) {
                wr(r1 + 2 * x * bps, u, f);
                wr(r1 + (2 * x + 1) * bps, v, f);
            } else {
                wr(r1 + x * bps, u, f);
                wr(cv->im.data[2] + y * cv->im.stride[2] + x * bps, v, f);
            }
        }
}

static void from_fmt(const conv *cv, const tfmt *f, uint8_t *dst, int w, int h)
{
    uint8_t *Y = dst, *U = dst + (size_t)w * h, *V = U + (size_t)(w / 2) * (h / 2);
    int bps = f->bps;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            Y[y * w + x] = (uint8_t)rd(cv->im.data[0] + y * cv->im.stride[0] + x * bps, f);
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            const uint8_t *r1 = cv->im.data[1] + y * cv->im.stride[1];
            if (f->nv) {
                U[y * (w / 2) + x] = (uint8_t)rd(r1 + 2 * x * bps, f);
                V[y * (w / 2) + x] = (uint8_t)rd(r1 + (2 * x + 1) * bps, f);
            } else {
                U[y * (w / 2) + x] = (uint8_t)rd(r1 + x * bps, f);
                V[y * (w / 2) + x] = (uint8_t)rd(cv->im.data[2] + y * cv->im.stride[2] + x * bps, f);
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

static void dump_fields(mb_ctx *c, const char *path, int fw, int fh)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    float *f = xalloc(sizeof(float) * 4 * (size_t)((fw + 7) / 8) * ((fh + 7) / 8));
    int w, h;
    for (int which = 0; which < 3; which++) {
        if (mb_debug_field(c, which, f, &w, &h) < 0)
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
    fprintf(stderr, "eval in.y4m|in.mkv [-n frames] [-skip k] [-step s] [-mode motionblade|blend|repeat] [-cmp full.y4m]\n"
                    "     [-o out.y4m] [-p name=value]... [-q] [-export] [-fmt ");
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
        fprintf(stderr, "%s%s", i ? "|" : "", fmts[i].name);
    fprintf(stderr, "] [-dump frame[:fields.ppm]]\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out = NULL, *cmp = NULL, *mode = "motionblade", *dumpf = NULL;
    int maxn = 1000, step = 2, quiet = 0, skip = 0, nometric = 0, repeat = 1, fm = 0, dumpk = -1, xport = 0;
    char *params[64];
    char dumpbuf[4096];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) maxn = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-skip") && i + 1 < argc) skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-step") && i + 1 < argc) step = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "-cmp") && i + 1 < argc) cmp = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc && np < 64) params[np++] = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc) {
            dumpk = atoi(argv[++i]);
            dumpf = strchr(argv[i], ':');
            if (dumpf)
                dumpf++;
        }
        else if (!strcmp(argv[i], "-fmt") && i + 1 < argc) {
            i++;
            for (size_t j = 0; j < sizeof(fmts) / sizeof(fmts[0]); j++)
                if (!strcmp(argv[i], fmts[j].name))
                    fm = (int)j;
        }
        else if (!strcmp(argv[i], "-repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-nometric")) nometric = 1, quiet = 1;
        else if (!strcmp(argv[i], "-export")) xport = 1;
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
    if (cmp && (sc.w != s.w || sc.h != s.h)) {
        fprintf(stderr, "%s and %s differ in size\n", in, cmp);
        return 1;
    }
    int w = s.w, h = s.h;
    size_t ysz = (size_t)w * h, fsz = frame_size(w, h);
    const tfmt *tf = &fmts[fm];
    mb_format fmt = {w, h, tf->depth, 1, 1, tf->nv, tf->lsb_pad};
    conv ca, cb, co;
    if (fm) {
        conv_alloc(&ca, tf, w, h);
        conv_alloc(&cb, tf, w, h);
        conv_alloc(&co, tf, w, h);
    }
    if (dumpk >= 0 && !dumpf && out) {
        snprintf(dumpbuf, sizeof(dumpbuf), "%s.fields.ppm", out);
        dumpf = dumpbuf;
    } else if (dumpk >= 0 && !dumpf) {
        dumpf = "/tmp/motionblade-out/fields.ppm";
    }
    mb_ctx *c = mb_create(&fmt);
    if (!c) {
        fprintf(stderr, "mb_create failed\n");
        return 1;
    }
    if (xport && mb_export_init(c) < 0) {
        fprintf(stderr, "dma-buf export not available\n");
        return 1;
    }
    int la = 1;
    for (int i = 0; i < np; i++) {
        char *eq = strchr(params[i], '=');
        if (!eq) usage();
        *eq = 0;
        if (!strcmp(params[i], "la"))
            la = atof(eq + 1) != 0;
        if (mb_set_param(c, params[i], atof(eq + 1)) < 0) {
            fprintf(stderr, "unknown param %s\n", params[i]);
            return 1;
        }
    }
    FILE *fo = out ? fopen(out, "wb") : NULL;
    if (fo)
        fputs(s.hdr, fo);
    uint8_t *res = xalloc(fsz);
    double swb = 0, spc = 0, sp = 0, ss = 0, sit = 0, worst = 99;
    int cnt = 0, worsti = -1;
    double wall0 = now_ms();
    for (int rep_i = 0; rep_i < repeat; rep_i++)
    for (int k = 0; (k + 1) * step < s.n; k++) {
        if (k == 0 && rep_i > 0)
            mb_reset(c);
        uint8_t *A = s.f[k * step], *B = s.f[(k + 1) * step];
        if (!strcmp(mode, "motionblade")) {
            for (int j = k == 0 ? 0 : k + 1 + la; j <= k + 1 + la; j++) {
                if (j * step >= s.n) {
                    mb_flush(c);
                    continue;
                }
                uint8_t *P = s.f[j * step];
                if (fm) {
                    to_fmt(&cb, tf, P, w, h);
                    mb_push(c, &cb.im);
                } else {
                    mb_image ip = yuv420(P, w, h);
                    mb_push(c, &ip);
                }
            }
        }
        if (fo)
            fprintf(fo, "FRAME\n"), fwrite(A, 1, fsz, fo);
        for (int m = 1; m < step; m++) {
            double t = (double)m / step;
            const uint8_t *R = res;
            mb_stats st = {0};
            if (cmp) {
                if (k * step + m >= sc.n) {
                    fprintf(stderr, "%s has only %d frames\n", cmp, sc.n);
                    return 1;
                }
                R = sc.f[k * step + m];
            } else if (!strcmp(mode, "motionblade")) {
                mb_image io = yuv420(res, w, h);
                if (mb_interpolate(c, t, fm ? &co.im : &io) < 0) {
                    fprintf(stderr, "interpolation failed at frame %d\n", k * step + m);
                    return 1;
                }
                if (fm)
                    from_fmt(&co, tf, res, w, h);
                mb_get_stats(c, &st);
            } else if (!strcmp(mode, "blend")) {
                for (size_t i = 0; i < fsz; i++)
                    res[i] = (uint8_t)lrint(A[i] * (1 - t) + B[i] * t);
            } else {
                memcpy(res, t < 0.5 ? A : B, fsz);
            }
            if (k * step + m == dumpk && !strcmp(mode, "motionblade")) {
                dump_fields(c, dumpf, w, h);
            }
            const uint8_t *T = s.f[k * step + m];
            double p = nometric ? 0 : psnr(R, T, ysz), q = nometric ? 0 : ssim(R, T, w, h);
            double wb = nometric ? 0 : worst_blocks(R, T, w, h);
            if (!quiet)
                printf("frame %3d  local %6.2f  psnr %6.2f  ssim %.4f  gpu %6.2fms  err %.1f%s\n",
                       k * step + m, wb, p, q, st.interp_ms, st.mean_err, st.scene_cut ? " CUT" : "");
            sp += p;
            spc += nometric ? 0 : psnr(R + ysz, T + ysz, ysz / 2);
            swb += wb;
            ss += q;
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
    double wall1 = now_ms();
    if (cnt == 0) {
        fprintf(stderr, "no frame interpolated: %d frames read, need more than %d with -step %d\n", s.n, step, step);
        return 1;
    }
    if (nometric)
        printf("wall %.2f ms per interpolated frame\n", (wall1 - wall0) / cnt);
    printf("MEAN local %.3f | chroma psnr %.3f | ", swb / cnt, spc / cnt);
    printf("MEAN psnr %.3f ssim %.4f  worst %.2f@%d  gpu %.2fms  (%d frames)\n", sp / cnt, ss / cnt, worst, worsti,
           sit / cnt, cnt);
    if (fo)
        fclose(fo);
    mb_destroy(c);
    return 0;
}
