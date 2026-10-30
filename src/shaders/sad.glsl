#extension GL_KHR_shader_subgroup_clustered : enable
#extension GL_KHR_shader_subgroup_basic : enable

float xr[16];
vec2 g_inv;
vec2 g_row0;
float g_w = 1.0;
float g_ml, g_sad, g_zsad;
float best_c, best_s, best_m;
vec2 best_v;

#define ROW_FN(NAME, N)                                                                        \
    void NAME(sampler2D T, vec2 p, out float o[N])                                             \
    {                                                                                          \
        vec2 q = p - 0.5;                                                                      \
        vec2 fl = floor(q);                                                                    \
        vec2 f = q - fl;                                                                       \
        vec2 base = (fl + 1.0) * g_inv;                                                        \
        float top[N + 2], bot[N + 2];                                                          \
        for (int k = 0; k < (N + 2) / 2; k++) {                                                \
            vec4 g = textureGather(T, base + vec2(float(2 * k) * g_inv.x, 0.0));               \
            top[2 * k] = g.w;                                                                  \
            top[2 * k + 1] = g.z;                                                              \
            bot[2 * k] = g.x;                                                                  \
            bot[2 * k + 1] = g.y;                                                              \
        }                                                                                      \
        for (int x = 0; x < N; x++)                                                            \
            o[x] = mix(mix(top[x], top[x + 1], f.x), mix(bot[x], bot[x + 1], f.x), f.y);       \
    }

#define HW_FN(NAME, N)                                                             \
    void NAME(sampler2D T, vec2 p, out float o[N])                                 \
    {                                                                              \
        for (int x = 0; x < N; x++)                                                \
            o[x] = textureLod(T, (p + vec2(float(x), 0.0)) * g_inv, 0.0).r;        \
    }

#ifdef HW_ROW
HW_FN(row16, 16)
HW_FN(row18, 18)
HW_FN(row24, 24)
#else
ROW_FN(row16, 16)
#endif

float zsad(float a[16], float b[16])
{
    float ml = 0.0;
    for (int x = 0; x < 16; x++)
        ml += a[x] - b[x];
    g_ml = ml;
    float m = 0.0;
    if (g_w > 0.0)
        m = subgroupClusteredAdd(ml, 16) / 256.0 * g_w;
    float s = 0.0;
    for (int x = 0; x < 16; x++)
        s += abs(a[x] - b[x] - m);
#ifdef PLAIN_SAD
    float mf = subgroupClusteredAdd(ml, 16) / 256.0;
    float s0 = 0.0, s1 = 0.0;
    for (int x = 0; x < 16; x++) {
        s0 += abs(a[x] - b[x]);
        s1 += abs(a[x] - b[x] - mf);
    }
    g_sad = subgroupClusteredAdd(s0, 16) * 255.0;
    g_zsad = subgroupClusteredAdd(s1, 16) * 255.0;
#endif
    return subgroupClusteredAdd(s, 16) * 255.0;
}

float block_sum()
{
    float s = 0.0;
    for (int x = 0; x < 16; x++)
        s += xr[x];
    return subgroupClusteredAdd(s, 16);
}

float block_dc(float ml)
{
    return subgroupClusteredAdd(ml, 16) * (255.0 / 256.0);
}

void set_dc_w(float m)
{
    g_w = smoothstep(pc.f3.z, pc.f3.w, subgroupClusteredMin(m, 16));
}

void load_x(sampler2D X, int x0, int y0, int lane)
{
    g_row0 = vec2(x0, y0 + lane) + 0.5;
    row16(X, g_row0, xr);
}

float sad_y(sampler2D Y, vec2 v)
{
    float yr[16];
    row16(Y, g_row0 + v, yr);
    return zsad(xr, yr);
}

void consider(vec2 u, float s, float c)
{
    if (c < best_c) {
        best_c = c;
        best_s = s;
        best_m = g_ml;
        best_v = u;
    }
}

void try_v(sampler2D Y, vec2 u, float pen)
{
    float s = sad_y(Y, u);
    consider(u, s, s + pen + pc.f1.y * l1(u));
}
