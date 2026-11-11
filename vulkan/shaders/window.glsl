#extension GL_KHR_shader_subgroup_clustered : enable
#extension GL_KHR_shader_subgroup_basic : enable

#define LANES MB_WINDOW_SIZE

float origin_row[LANES];
vec2 texel_size;
vec2 lane_start;
float mean_weight = 1.0;
float lane_diff;
float plain_sad;
float zero_mean_sad;

struct Best {
    float cost;
    float sad;
    float diff;
    vec2 vector;
};

Best best;

#define EXACT_ROW(NAME, N)                                                                     \
    void NAME(sampler2D T, vec2 p, out float row[N])                                           \
    {                                                                                          \
        vec2 q = p - 0.5;                                                                      \
        vec2 corner = floor(q);                                                                \
        vec2 f = q - corner;                                                                   \
        vec2 base = (corner + 1.0) * texel_size;                                               \
        float top[N + 2], bottom[N + 2];                                                       \
        for (int k = 0; k < (N + 2) / 2; k++) {                                                \
            vec4 g = textureGather(T, base + vec2(float(2 * k) * texel_size.x, 0.0));          \
            top[2 * k] = g.w;                                                                  \
            top[2 * k + 1] = g.z;                                                              \
            bottom[2 * k] = g.x;                                                               \
            bottom[2 * k + 1] = g.y;                                                           \
        }                                                                                      \
        for (int x = 0; x < N; x++) {                                                          \
            row[x] = mix(mix(top[x], top[x + 1], f.x), mix(bottom[x], bottom[x + 1], f.x), f.y); \
        }                                                                                      \
    }

#define HARDWARE_ROW(NAME, N)                                                      \
    void NAME(sampler2D T, vec2 p, out float row[N])                               \
    {                                                                              \
        for (int x = 0; x < N; x++) {                                              \
            row[x] = textureLod(T, (p + vec2(float(x), 0.0)) * texel_size, 0.0).r; \
        }                                                                          \
    }

#ifdef HARDWARE_ROWS
HARDWARE_ROW(load_row, 16)
HARDWARE_ROW(load_row18, 18)
#else
EXACT_ROW(load_row, 16)
#endif

float cluster_sum(float value)
{
    return subgroupClusteredAdd(value, LANES);
}

float window_sad(float a[LANES], float b[LANES])
{
    float diff = 0.0;
    for (int x = 0; x < LANES; x++) {
        diff += a[x] - b[x];
    }
    lane_diff = diff;
    float mean = 0.0;
    if (mean_weight > 0.0) {
        mean = cluster_sum(diff) / 256.0 * mean_weight;
    }
    float sum = 0.0;
    for (int x = 0; x < LANES; x++) {
        sum += abs(a[x] - b[x] - mean);
    }
#ifdef SAD_STATISTICS
    float full_mean = cluster_sum(diff) / 256.0;
    float plain = 0.0, zero_mean = 0.0;
    for (int x = 0; x < LANES; x++) {
        plain += abs(a[x] - b[x]);
        zero_mean += abs(a[x] - b[x] - full_mean);
    }
    plain_sad = cluster_sum(plain) * 255.0;
    zero_mean_sad = cluster_sum(zero_mean) * 255.0;
#endif
    return cluster_sum(sum) * 255.0;
}

float origin_sum()
{
    float sum = 0.0;
    for (int x = 0; x < LANES; x++) {
        sum += origin_row[x];
    }
    return cluster_sum(sum);
}

float block_dc(float diff)
{
    return cluster_sum(diff) * (255.0 / 256.0);
}

void set_mean_weight(float lo, float hi, float lane_dc)
{
    mean_weight = smoothstep(lo, hi, subgroupClusteredMin(lane_dc, LANES));
}

void load_origin(sampler2D origin, ivec2 corner, int lane)
{
    lane_start = vec2(corner.x, corner.y + lane) + 0.5;
    load_row(origin, lane_start, origin_row);
}

float target_sad(sampler2D target, vec2 v)
{
    float row[LANES];
    load_row(target, lane_start + v, row);
    return window_sad(origin_row, row);
}

void reset_best()
{
    best = Best(1e30, 0.0, 0.0, vec2(0.0));
}

void consider(vec2 u, float sad, float cost)
{
    if (cost < best.cost) {
        best = Best(cost, sad, lane_diff, u);
    }
}

#ifdef SPARSE_STRIDE
#define SPARSE (LANES / SPARSE_STRIDE)

float sparse_origin[SPARSE];

int sparse_phase()
{
    return int(gl_LocalInvocationID.x) & (SPARSE_STRIDE - 1);
}

#define SPARSE_ROW(NAME, N)                                                                         \
    void NAME(sampler2D T, vec2 p, out float row[N])                                                \
    {                                                                                               \
        float phase = float(sparse_phase());                                                        \
        for (int k = 0; k < N; k++) {                                                               \
            row[k] = textureLod(T, (p + vec2(float(SPARSE_STRIDE * k) + phase, 0.0)) * texel_size, 0.0).r; \
        }                                                                                           \
    }

SPARSE_ROW(load_sparse, SPARSE)

void load_sparse_origin()
{
    int phase = sparse_phase();
    for (int k = 0; k < SPARSE; k++) {
        float value = origin_row[SPARSE_STRIDE * k];
        for (int j = 1; j < SPARSE_STRIDE; j++) {
            value = phase == j ? origin_row[SPARSE_STRIDE * k + j] : value;
        }
        sparse_origin[k] = value;
    }
}

float sparse_window_sad(float row[SPARSE])
{
    float diff = 0.0;
    for (int x = 0; x < SPARSE; x++) {
        diff += sparse_origin[x] - row[x];
    }
    lane_diff = diff * float(SPARSE_STRIDE);
    float mean = 0.0;
    if (mean_weight > 0.0) {
        mean = cluster_sum(diff) / float(LANES * SPARSE) * mean_weight;
    }
    float sum = 0.0;
    for (int x = 0; x < SPARSE; x++) {
        sum += abs(sparse_origin[x] - row[x] - mean);
    }
    return cluster_sum(sum) * 255.0 * float(SPARSE_STRIDE);
}

void try_sparse(sampler2D target, vec2 u, float penalty, float length_penalty)
{
    float row[SPARSE];
    load_sparse(target, lane_start + u, row);
    float sad = sparse_window_sad(row);
    consider(u, sad, sad + penalty + length_penalty * l1(u));
}

void measure_best(sampler2D target)
{
    best.sad = target_sad(target, best.vector);
    best.diff = lane_diff;
}
#endif
