#ifndef MB_WIPE_COMMON_H
#define MB_WIPE_COMMON_H

#include "common/vec.h"
#include "motionblade/layout.h"
#include "motionblade/passes.h"
#include "reference/cpu_resources.h"

enum { CELL = MB_WIPE_CELL, WIPE_OLD = MB_WIPE_OLD, WIPE_NEW = MB_WIPE_NEW, WIPE_CHANGES = MB_WIPE_CHANGES };
enum { WIPE_BEFORE = 0, WIPE_FROM = 1, WIPE_TO = 2, WIPE_AFTER = 3 };

static inline float wipe_luma(const mb_image *image, int x, int y)
{
    return ref_texel(&image->texture, x, y, 0) * 255.0f;
}

static inline bool wipe_texel_inside(mb_wipe_grid grid, int x, int y)
{
    return x >= 0 && y >= 0 && x < grid.width && y < grid.height;
}

static inline bool wipe_cell_inside(mb_wipe_grid grid, int x, int y)
{
    return x >= 0 && y >= 0 && x < grid.cells_x && y < grid.cells_y;
}

static inline int wipe_slot(int kind, int axis, bool positive)
{
    return kind * 4 + axis * 2 + (positive ? 1 : 0);
}

static inline bool wipe_gate_open(const mb_buffer *gate, int stage, unsigned minimum)
{
    return words(gate)[stage] >= minimum;
}

#endif
