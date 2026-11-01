#include "common/blocks.h"
#include "estimation/window.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

enum { NEIGHBOURS = 9, CELL_PIXELS = MB_CELL_SIZE * MB_CELL_SIZE };

typedef struct cell_candidates {
    vec2 vectors[NEIGHBOURS];
    int count;
    bool at_edge;
} cell_candidates;

static cell_candidates gather(const mb_split_pass *pass, ivec2 block, vec2 own)
{
    cell_candidates list = {.count = 0, .at_edge = false};
    const vec4 *selection = vec4s(pass->selection);
    for (int k = 0; k < NEIGHBOURS; k++) {
        vec4 q = selection[neighbour3_index(k, block, pass->grid)];
        if ((int)q.z != MB_MODE_TWO_SIDED) {
            continue;
        }
        vec2 v = v4_xy(q);
        list.at_edge = list.at_edge || v2_l1(v2_sub(v, own)) > pass->threshold;
        if (!v2_listed(list.vectors, list.count, v)) {
            list.vectors[list.count++] = v;
        }
    }
    return list;
}

typedef struct cell_job {
    const mb_split_pass *pass;
    ivec2 cell;
    float dc;
} cell_job;

static float cell_cost(const cell_job *job, vec2 u)
{
    const mb_split_pass *pass = job->pass;
    ivec2 cell = job->cell;
    float dc = job->dc;
    const ref_texture *a = &pass->a->texture;
    const ref_texture *b = &pass->b->texture;
    vec2 inv = v2_inverse_size(pass->grid.width, pass->grid.height);
    float lanes[CELL_PIXELS];
    for (int lane = 0; lane < CELL_PIXELS; lane++) {
        ivec2 pixel = iv2(cell.x * MB_CELL_SIZE + (lane & 3), cell.y * MB_CELL_SIZE + (lane >> 2));
        vec2 p = v2((float)pixel.x + 0.5f, (float)pixel.y + 0.5f);
        vec2 pa = v2_sub(p, v2_scale(u, pass->t));
        vec2 pb = v2_add(p, v2_scale(u, 1.0f - pass->t));
        float va = ref_sample(a, pa.x * inv.x, pa.y * inv.y, 0);
        float vb = ref_sample(b, pb.x * inv.x, pb.y * inv.y, 0);
        lanes[lane] = fabsf(va - vb - dc);
    }
    return cluster_sum(lanes) * 255.0f;
}

static vec2 best_cell_vector(const cell_job *job, vec2 own, const cell_candidates *list)
{
    const mb_split_pass *pass = job->pass;
    float best = 1e30f;
    vec2 chosen = own;
    for (int i = 0; i < list->count; i++) {
        float cost = cell_cost(job, list->vectors[i]);
        if (cost < best) {
            best = cost;
            chosen = list->vectors[i];
        }
    }
    for (int r = pass->refine_radius; r > 0; r--) {
        vec2 center = chosen;
        for (int k = 0; k < 8; k++) {
            vec2 u = v2_add(center, v2_scale(v2((float)RING8[k].x, (float)RING8[k].y), (float)r));
            float cost = cell_cost(job, u) + pass->refine_penalty * v2_l1(v2_sub(u, center));
            if (cost < best) {
                best = cost;
                chosen = u;
            }
        }
    }
    return chosen;
}

void ref_split(const mb_split_pass *pass)
{
    const vec4 *selection = vec4s(pass->selection);
    const vec4 *offsets = vec4s(pass->offsets);
    vec4 *cells = vec4s(pass->cells);
    int cells_x = pass->grid.blocks_x * 2;
    for (int id = 0; id < cells_x * pass->grid.blocks_y * 2; id++) {
        ivec2 cell = iv2(id % cells_x, id / cells_x);
        ivec2 block = iv2(cell.x >> 1, cell.y >> 1);
        int index = block_index(block, pass->grid);
        vec4 own = selection[index];
        cells[id] = own;
        if ((int)own.z != MB_MODE_TWO_SIDED || pass->threshold <= 0.0f) {
            continue;
        }
        cell_candidates list = gather(pass, block, v4_xy(own));
        if (list.at_edge) {
            cell_job job = {pass, cell, offsets[index].x * pass->luma_scale};
            vec2 v = best_cell_vector(&job, v4_xy(own), &list);
            cells[id] = (vec4){v.x, v.y, 0.0f, own.w};
        }
    }
}
