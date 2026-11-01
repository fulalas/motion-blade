#include <math.h>
#include <stdlib.h>

#include "common/blocks.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { MAX_RADIUS = 16, WINDOW = MB_WINDOW_SIZE, WINDOW_PIXELS = WINDOW * WINDOW };

typedef struct search_area {
    float origin[WINDOW_PIXELS];
    float target[(WINDOW + 2 * MAX_RADIUS) * (WINDOW + 2 * MAX_RADIUS)];
    int radius;
    int span;
} search_area;

typedef struct match {
    int index;
    float cost;
} match;

static void load_area(search_area *area, const mb_full_search_pass *pass, ivec2 origin)
{
    const ref_texture *x = &pass->origin->texture;
    const ref_texture *y = &pass->target->texture;
    for (int i = 0; i < WINDOW_PIXELS; i++) {
        area->origin[i] = ref_texel(x, origin.x + i % WINDOW, origin.y + i / WINDOW, 0) * 255.0f;
    }
    int r = area->radius;
    for (int i = 0; i < area->span * area->span; i++) {
        area->target[i] = ref_texel(y, origin.x - r + i % area->span, origin.y - r + i / area->span, 0) * 255.0f;
    }
}

static float candidate_sad(const search_area *area, int vx, int vy)
{
    float sum = 0.0f;
    for (int y = 0; y < WINDOW; y++) {
        for (int x = 0; x < WINDOW; x++) {
            sum += fabsf(area->origin[y * WINDOW + x] - area->target[(y + vy) * area->span + x + vx]);
        }
    }
    return sum;
}

static match best_match(const search_area *area, float length_penalty)
{
    int diameter = 2 * area->radius + 1;
    match best = {0, 1e30f};
    for (int c = 0; c < diameter * diameter; c++) {
        int vx = c % diameter;
        int vy = c / diameter;
        float cost = candidate_sad(area, vx, vy);
        cost += length_penalty * (float)(abs(vx - area->radius) + abs(vy - area->radius));
        if (cost < best.cost) {
            best = (match){c, cost};
        }
    }
    return best;
}

static float tree_sum(float values[WINDOW_PIXELS])
{
    for (int stride = WINDOW_PIXELS / 2; stride > 0; stride >>= 1) {
        for (int i = 0; i < stride; i++) {
            values[i] += values[i + stride];
        }
    }
    return values[0];
}

static float mean_difference(const search_area *area, int vx, int vy)
{
    float diff[WINDOW_PIXELS];
    for (int i = 0; i < WINDOW_PIXELS; i++) {
        diff[i] = area->origin[i] - area->target[(i / WINDOW + vy) * area->span + i % WINDOW + vx];
    }
    return tree_sum(diff) / (float)WINDOW_PIXELS;
}

void ref_full_search(const mb_full_search_pass *pass)
{
    search_area storage;
    search_area *area = &storage;
    area->radius = pass->radius < MAX_RADIUS ? pass->radius : MAX_RADIUS;
    area->span = WINDOW + 2 * area->radius;
    int diameter = 2 * area->radius + 1;
    vec4 *field = vec4s(pass->field);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        load_area(area, pass, window_origin(block_coord(id, pass->grid)));
        match best = best_match(area, pass->length_penalty);
        int vx = best.index % diameter;
        int vy = best.index / diameter;
        float mean = mean_difference(area, vx, vy);
        field[id] = (vec4){(float)(vx - area->radius), (float)(vy - area->radius), best.cost, mean};
    }
}
