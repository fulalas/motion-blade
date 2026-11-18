#include "reference/kernels.h"
#include "wipe/common.h"

enum { WINDOW = MB_WIPE_CELL + MB_WIPE_GAP + 1 };
enum { SIDE_EARLIER = 1, SIDE_LATER = 2, SIDE_BOTH = 3 };
enum { CHANGED_BEFORE = 1, CHANGED_NOW = 2, CHANGED_AFTER = 4 };

typedef struct cell_window {
    unsigned changed[WINDOW * WINDOW];
    float luma[4][WINDOW * WINDOW];
} cell_window;

typedef struct seed_sum {
    vec2 position;
    vec2 direction;
    float count;
    float outlined;
} seed_sum;

static unsigned changes_at(const mb_wipe_seeds_pass *pass, int x, int y, float luma[4])
{
    for (int k = 0; k < 4; k++) {
        luma[k] = wipe_luma(pass->frames[k], x, y);
    }
    float limit = (float)MB_WIPE_CHANGE;
    return (fabsf(luma[WIPE_FROM] - luma[WIPE_BEFORE]) > limit ? CHANGED_BEFORE : 0u) |
           (fabsf(luma[WIPE_TO] - luma[WIPE_FROM]) > limit ? CHANGED_NOW : 0u) |
           (fabsf(luma[WIPE_AFTER] - luma[WIPE_TO]) > limit ? CHANGED_AFTER : 0u);
}

static void read_window(const mb_wipe_seeds_pass *pass, ivec2 origin, cell_window *w)
{
    for (int i = 0; i < WINDOW * WINDOW; i++) {
        int x = origin.x + i % WINDOW;
        int y = origin.y + i / WINDOW;
        float luma[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        w->changed[i] = wipe_texel_inside(pass->grid, x, y) ? changes_at(pass, x, y, luma) : 0u;
        for (int k = 0; k < 4; k++) {
            w->luma[k][i] = luma[k];
        }
    }
}

static int side_at(const cell_window *w, int kind, int index)
{
    unsigned bits = w->changed[index] >> kind;
    return (int)(bits & 3u);
}

static bool is_lasting(const cell_window *w, int kind, int first, int last)
{
    float edge = fabsf(w->luma[kind + 1][first] - w->luma[kind + 1][last]);
    float before = fabsf(w->luma[kind][first] - w->luma[kind][last]);
    float after = fabsf(w->luma[kind + 2][first] - w->luma[kind + 2][last]);
    return edge < (float)MB_WIPE_MIN_STEP || fmaxf(before, after) > (float)MB_WIPE_LASTING * edge;
}

static bool is_torn(const cell_window *w, int kind, int earlier, int later)
{
    float edge = fabsf(w->luma[kind + 1][earlier] - w->luma[kind + 1][later]);
    return fabsf(w->luma[WIPE_FROM][earlier] - w->luma[WIPE_TO][later]) > (float)MB_WIPE_LASTING * edge;
}

static bool is_outlined(const cell_window *w, int kind, int first, int last, int step)
{
    const float *shown = w->luma[kind + 1];
    float lo = fminf(shown[first], shown[last]) - (float)MB_WIPE_OUTLINE;
    float hi = fmaxf(shown[first], shown[last]) + (float)MB_WIPE_OUTLINE;
    bool outlined = is_lasting(w, kind, first, last);
    for (int i = first + step; i != last; i += step) {
        outlined = outlined || shown[i] < lo || shown[i] > hi;
    }
    return outlined;
}

static void add_front(seed_sum *sum, vec2 start, ivec2 step, int gap, float sign, bool outlined)
{
    float reach = 0.5f * (float)(gap + 1);
    sum->position = v2_add(sum->position, v2(start.x + reach * (float)step.x, start.y + reach * (float)step.y));
    sum->direction = v2_add(sum->direction, v2(sign * (float)step.x, sign * (float)step.y));
    sum->count += 1.0f;
    sum->outlined += outlined ? 1.0f : 0.0f;
}

static void find_front(const cell_window *w, int kind, ivec2 p, ivec2 step, vec2 start, seed_sum *sum)
{
    int first = p.y * WINDOW + p.x;
    int stride = step.y * WINDOW + step.x;
    int side = side_at(w, kind, first);
    if (side != SIDE_EARLIER && side != SIDE_LATER) {
        return;
    }
    for (int gap = 0; gap <= MB_WIPE_GAP; gap++) {
        int last = first + (gap + 1) * stride;
        int other = side_at(w, kind, last);
        if (other == SIDE_BOTH) {
            continue;
        }
        if (other + side == SIDE_EARLIER + SIDE_LATER) {
            bool forward = side == SIDE_EARLIER;
            bool torn = is_torn(w, kind, forward ? first : last, forward ? last : first);
            add_front(sum, start, step, gap, forward ? 1.0f : -1.0f, torn || is_outlined(w, kind, first, last, stride));
        }
        return;
    }
}

static float direction_code(seed_sum s)
{
    if (s.count < (float)MB_WIPE_MIN_POINTS || s.outlined > (float)MB_WIPE_MAX_OUTLINED * s.count) {
        return 0.0f;
    }
    float ax = fabsf(s.direction.x);
    float ay = fabsf(s.direction.y);
    if (ax + ay < (float)MB_WIPE_PURITY * s.count) {
        return 0.0f;
    }
    if (ax > 0.0f && ax >= (float)MB_WIPE_DOMINANCE * ay) {
        return s.direction.x > 0.0f ? 1.0f : -1.0f;
    }
    if (ay > 0.0f && ay >= (float)MB_WIPE_DOMINANCE * ax) {
        return s.direction.y > 0.0f ? 2.0f : -2.0f;
    }
    return 0.0f;
}

static vec4 seed_of(seed_sum s)
{
    float scale = 1.0f / fmaxf(s.count, 1.0f);
    return (vec4){s.position.x * scale, s.position.y * scale, direction_code(s), 0.0f};
}

static void seed_cell(const mb_wipe_seeds_pass *pass, int cell)
{
    ivec2 origin = iv2(cell % pass->grid.cells_x * CELL, cell / pass->grid.cells_x * CELL);
    cell_window w;
    read_window(pass, origin, &w);
    seed_sum sums[2] = {{v2(0.0f, 0.0f), v2(0.0f, 0.0f), 0.0f, 0.0f}, {v2(0.0f, 0.0f), v2(0.0f, 0.0f), 0.0f, 0.0f}};
    float changed[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < CELL * CELL; i++) {
        ivec2 p = iv2(i % CELL, i / CELL);
        vec2 start = v2((float)(origin.x + p.x) + 0.5f, (float)(origin.y + p.y) + 0.5f);
        for (int k = 0; k < 3; k++) {
            changed[k] += (w.changed[p.y * WINDOW + p.x] >> k & 1u) != 0u ? 1.0f : 0.0f;
        }
        for (int kind = 0; kind < 2; kind++) {
            find_front(&w, kind, p, iv2(1, 0), start, &sums[kind]);
            find_front(&w, kind, p, iv2(0, 1), start, &sums[kind]);
        }
    }
    vec4 *seeds = vec4s(pass->seeds) + MB_WIPE_SEED_VALUES * cell;
    seeds[WIPE_OLD] = seed_of(sums[WIPE_OLD]);
    seeds[WIPE_NEW] = seed_of(sums[WIPE_NEW]);
    seeds[WIPE_CHANGES] = (vec4){changed[0], changed[1], changed[2], 0.0f};
    if (seeds[WIPE_OLD].z != 0.0f || seeds[WIPE_NEW].z != 0.0f) {
        words(pass->gate)[MB_WIPE_GATE_SEEDS] += 1u;
    }
}

void ref_wipe_seeds(const mb_wipe_seeds_pass *pass)
{
    for (int cell = 0; cell < pass->grid.cells_x * pass->grid.cells_y; cell++) {
        seed_cell(pass, cell);
    }
}
