layout(push_constant) uniform PC {
    ivec4 i0, i1, i2, i3;
    vec4 f0, f1, f2, f3;
} pc;

#define M_FUT 1
#define M_PAST 2
#define M_UNC 3

float l1(vec2 a) { return abs(a.x) + abs(a.y); }

float cons_thr(float cthr, vec2 u) { return cthr * 0.25 + l1(u) / 8.0; }

int bidx(ivec2 b, ivec2 n)
{
    b = clamp(b, ivec2(0), n - 1);
    return b.y * n.x + b.x;
}

const ivec2 DIR8[8] = ivec2[8](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
                               ivec2(-1, -1), ivec2(1, -1), ivec2(-1, 1), ivec2(1, 1));

ivec2 off3(int k) { return ivec2(k % 3 - 1, k / 3 - 1); }

int nbr3(int lane, ivec2 b, ivec2 n)
{
    return bidx(b + off3(lane), n);
}

vec2 chroma2(sampler2D T1, sampler2D T2, vec2 p, bool nv)
{
    return nv ? textureLod(T1, p, 0.0).rg : vec2(textureLod(T1, p, 0.0).r, textureLod(T2, p, 0.0).r);
}
