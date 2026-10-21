#extension GL_KHR_shader_subgroup_clustered : enable
#extension GL_KHR_shader_subgroup_basic : enable

float xr[16];
vec2 g_inv;
vec2 g_row0;
float g_w = 1.0;
float g_ml, g_sad, g_zsad;

void row16(sampler2D T, vec2 p, out float o[16])
{
    vec2 q = p - 0.5;
    vec2 fl = floor(q);
    vec2 f = q - fl;
    vec2 base = (fl + 1.0) * g_inv;
    float top[18], bot[18];
    for (int k = 0; k < 9; k++) {
        vec4 g = textureGather(T, base + vec2(float(2 * k) * g_inv.x, 0.0));
        top[2 * k] = g.w;
        top[2 * k + 1] = g.z;
        bot[2 * k] = g.x;
        bot[2 * k + 1] = g.y;
    }
    for (int x = 0; x < 16; x++)
        o[x] = mix(mix(top[x], top[x + 1], f.x), mix(bot[x], bot[x + 1], f.x), f.y);
}

float zsad(float a[16], float b[16])
{
    float ml = 0.0;
    for (int x = 0; x < 16; x++)
        ml += a[x] - b[x];
    g_ml = ml;
    float m = g_w > 0.0 ? subgroupClusteredAdd(ml, 16) / 256.0 * g_w : 0.0;
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

float block_dc(float ml)
{
    return subgroupClusteredAdd(ml, 16) * (255.0 / 256.0);
}

int nbr3(int lane, ivec2 b, ivec2 n)
{
    return bidx(b + ivec2(lane % 3 - 1, lane / 3 - 1), n);
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
