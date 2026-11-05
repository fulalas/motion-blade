#include "common/blocks.h"
#include "reference/cpu_resources.h"
#include "reference/kernels.h"

static void insert_key(uint32_t slots[MB_KEYS_PER_BLOCK], uint32_t key)
{
    for (int k = 0; k < MB_KEYS_PER_BLOCK && key != MB_KEY_NONE; k++) {
        uint32_t old = slots[k];
        slots[k] = old < key ? old : key;
        key = old > key ? old : key;
    }
}

void ref_project(const mb_project_pass *pass)
{
    const vec4 *field = vec4s(pass->field);
    uint32_t *keys = words(pass->keys);
    for (int id = 0; id < mb_grid_block_count(pass->grid); id++) {
        ivec2 block = block_coord(id, pass->grid);
        vec2 landing = v2_add(block_center(block), v2_scale(v4_xy(field[id]), pass->time_scale));
        ivec2 target = block_at(landing);
        if (block_inside(target, pass->grid)) {
            uint32_t key = (uint32_t)id * 2u + pass->tag;
            insert_key(&keys[block_index(target, pass->grid) * MB_KEYS_PER_BLOCK], key);
        }
    }
}
