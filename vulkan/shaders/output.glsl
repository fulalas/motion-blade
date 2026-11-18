float output_value(float v)
{
#ifdef DEPTH16
    v = min(v, push.max_value);
    if (push.quant_step > 0.0) {
        v = round(v / push.quant_step) * push.quant_step;
    }
#endif
    return v;
}

vec2 chroma_fetch(sampler2D first, sampler2D second, ivec2 p)
{
#ifdef NV
    return texelFetch(first, p, 0).rg;
#else
    return vec2(texelFetch(first, p, 0).r, texelFetch(second, p, 0).r);
#endif
}
