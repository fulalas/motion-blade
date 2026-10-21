layout(push_constant) uniform PC {
    ivec4 i0, i1, i2, i3;
    vec4 f0, f1, f2, f3;
} pc;

float l1(vec2 a) { return abs(a.x) + abs(a.y); }

int bidx(ivec2 b, ivec2 n)
{
    b = clamp(b, ivec2(0), n - 1);
    return b.y * n.x + b.x;
}
