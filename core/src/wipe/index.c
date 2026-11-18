#include <stdlib.h>

#include "reference/kernels.h"
#include "wipe/common.h"

static void index_line(const mb_wipe_index_pass *pass, int axis, int line)
{
    int length = axis == 0 ? pass->grid.cells_x : pass->grid.cells_y;
    for (int index = 0; index < MB_WIPE_SLOTS / 2; index++) {
        int kind = index / 2;
        bool positive = (index & 1) != 0;
        float code = (positive ? 1.0f : -1.0f) * (float)(axis + 1);
        int step = (kind == WIPE_NEW) == positive ? 1 : -1;
        int seen = -1;
        for (int i = 0; i < length; i++) {
            int along = step > 0 ? length - 1 - i : i;
            int cell = axis == 0 ? line * pass->grid.cells_x + along : along * pass->grid.cells_x + line;
            if (vec4s(pass->seeds)[MB_WIPE_SEED_VALUES * cell + kind].z == code) {
                seen = along;
            }
            bool near = seen >= 0 && abs(seen - along) <= MB_WIPE_RANGE;
            ((int *)pass->nearest->data)[MB_WIPE_SLOTS * cell + wipe_slot(kind, axis, positive)] = near ? seen : -1;
        }
    }
}

void ref_wipe_index(const mb_wipe_index_pass *pass)
{
    if (!wipe_gate_open(pass->gate, MB_WIPE_GATE_SEEDS, 1u)) {
        return;
    }
    for (int y = 0; y < pass->grid.cells_y; y++) {
        index_line(pass, 0, y);
    }
    for (int x = 0; x < pass->grid.cells_x; x++) {
        index_line(pass, 1, x);
    }
}
