vec2 chroma_sample(sampler2D first, sampler2D second, vec2 position, bool interleaved)
{
    if (interleaved) {
        return textureLod(first, position, 0.0).rg;
    }
    return vec2(textureLod(first, position, 0.0).r, textureLod(second, position, 0.0).r);
}
