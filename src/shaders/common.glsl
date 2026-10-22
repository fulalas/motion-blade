layout(push_constant) uniform PC {
    ivec4 i0, i1, i2, i3;
    vec4 f0, f1, f2, f3;
} pc;

#define M_FUT 1
#define M_PAST 2
#define M_UNC 3

float l1(vec2 a) { return abs(a.x) + abs(a.y); }

int bidx(ivec2 b, ivec2 n)
{
    b = clamp(b, ivec2(0), n - 1);
    return b.y * n.x + b.x;
}
