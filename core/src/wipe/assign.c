#include <stdlib.h>

#include "common/blocks.h"
#include "reference/kernels.h"
#include "wipe/common.h"

typedef struct front_line {
    float offset;
    float slope;
    bool found;
} front_line;

typedef struct sweep {
    int axis;
    int direction;
    front_line from;
    front_line to;
    float span;
} sweep;

static float along_of(vec2 p, int axis)
{
    return axis == 0 ? p.x : p.y;
}

static float lateral_of(vec2 p, int axis)
{
    return axis == 0 ? p.y : p.x;
}

static ivec2 cell_step(int axis, int along, int lateral)
{
    return axis == 0 ? iv2(along, lateral) : iv2(lateral, along);
}

static vec4 seed_at(const mb_wipe_assign_pass *pass, int kind, ivec2 cell)
{
    return vec4s(pass->seeds)[MB_WIPE_SEED_VALUES * (cell.y * pass->grid.cells_x + cell.x) + kind];
}

static bool has_seed(const mb_wipe_assign_pass *pass, int kind, ivec2 cell, float code)
{
    return wipe_cell_inside(pass->grid, cell.x, cell.y) && seed_at(pass, kind, cell).z == code;
}

static int lateral_offset(int k)
{
    return k % 2 ? -(k + 1) / 2 : k / 2;
}

static bool scan(const mb_wipe_assign_pass *pass, int kind, ivec2 cell, float code, ivec2 *found)
{
    if (!wipe_cell_inside(pass->grid, cell.x, cell.y)) {
        return false;
    }
    int axis = fabsf(code) > 1.5f ? 1 : 0;
    int slot = wipe_slot(kind, axis, code > 0.0f);
    int along = ((const int *)pass->nearest->data)[MB_WIPE_SLOTS * (cell.y * pass->grid.cells_x + cell.x) + slot];
    if (along < 0) {
        return false;
    }
    *found = axis == 0 ? iv2(along, cell.y) : iv2(cell.x, along);
    return true;
}

static void neighbour_seed(const mb_wipe_assign_pass *pass, int kind, ivec2 cell, int axis, int side, float code, vec2 *point)
{
    for (int k = 0; k < 3; k++) {
        ivec2 c = iv2_add(cell, cell_step(axis, lateral_offset(k), side));
        if (has_seed(pass, kind, c, code)) {
            *point = v4_xy(seed_at(pass, kind, c));
            return;
        }
    }
}

static front_line line_through(const mb_wipe_assign_pass *pass, int kind, ivec2 cell, int axis, float code)
{
    vec2 own = v4_xy(seed_at(pass, kind, cell));
    vec2 a = own;
    vec2 b = own;
    neighbour_seed(pass, kind, cell, axis, -1, code, &a);
    neighbour_seed(pass, kind, cell, axis, 1, code, &b);
    float lateral = lateral_of(b, axis) - lateral_of(a, axis);
    float along = along_of(b, axis) - along_of(a, axis);
    if (fabsf(lateral) <= 1e-3f || fabsf(along) > (float)MB_WIPE_MAX_SLOPE * fabsf(lateral)) {
        return (front_line){0.0f, 0.0f, false};
    }
    float slope = along / lateral;
    return (front_line){along_of(own, axis) - slope * lateral_of(own, axis), slope, true};
}

static bool is_blocked(const mb_wipe_assign_pass *pass, ivec2 cell, int axis, int from, int to)
{
    int direction = to >= from ? 1 : -1;
    for (int i = from; i != to + direction; i += direction) {
        ivec2 c = axis == 0 ? iv2(i, cell.y) : iv2(cell.x, i);
        vec4 changes = wipe_cell_inside(pass->grid, c.x, c.y) ? seed_at(pass, WIPE_CHANGES, c) : (vec4){0.0f, 0.0f, 0.0f, 0.0f};
        float before = changes.x;
        float after = changes.z;
        bool near_old = abs(i - from) < 2;
        bool near_new = abs(i - to) < 2;
        if ((!near_new && after >= (float)MB_WIPE_BLOCKING) || (!near_old && before >= (float)MB_WIPE_BLOCKING)) {
            return true;
        }
    }
    return false;
}

static float line_at(front_line line, float lateral)
{
    return line.offset + line.slope * lateral;
}

static front_line find_line(const mb_wipe_assign_pass *pass, int kind, ivec2 *cell, int axis, float code)
{
    front_line line = {0.0f, 0.0f, false};
    ivec2 start = *cell;
    for (int k = 0; k <= 2 * MB_WIPE_LATERAL && !line.found; k++) {
        if (scan(pass, kind, iv2_add(start, cell_step(axis, 0, lateral_offset(k))), code, cell)) {
            line = line_through(pass, kind, *cell, axis, code);
        }
    }
    return line;
}

static sweep sweep_through(const mb_wipe_assign_pass *pass, ivec2 cell, int axis, int direction)
{
    float code = (float)(direction * (axis + 1));
    ivec2 seed = cell;
    sweep s = {axis, direction, find_line(pass, WIPE_OLD, &seed, axis, code), {0.0f, 0.0f, false}, 0.0f};
    if (!s.from.found) {
        return s;
    }
    int first = axis == 0 ? seed.x : seed.y;
    s.to = find_line(pass, WIPE_NEW, &seed, axis, code);
    if (!s.to.found || is_blocked(pass, cell, axis, first, axis == 0 ? seed.x : seed.y)) {
        return s;
    }
    vec2 centre = v2(((float)cell.x + 0.5f) * (float)CELL, ((float)cell.y + 0.5f) * (float)CELL);
    float from = line_at(s.from, lateral_of(centre, axis));
    float to = line_at(s.to, lateral_of(centre, axis));
    float cell_lo = along_of(centre, axis) - 0.5f * (float)CELL;
    bool overlaps = fmaxf(fminf(from, to), cell_lo) <= fminf(fmaxf(from, to), cell_lo + (float)CELL);
    s.span = overlaps ? (to - from) * (float)direction : 0.0f;
    return s;
}

static bool is_shorter(sweep s, sweep best)
{
    return s.span >= (float)MB_WIPE_MIN_SPAN && (best.span <= 0.0f || s.span < best.span);
}

static sweep shortest_sweep(const mb_wipe_assign_pass *pass, ivec2 cell)
{
    sweep best = {0, 0, {0.0f, 0.0f, false}, {0.0f, 0.0f, false}, 0.0f};
    for (int k = 0; k < 4; k++) {
        sweep s = sweep_through(pass, cell, k / 2, k % 2 ? 1 : -1);
        if (is_shorter(s, best)) {
            best = s;
        }
    }
    return best;
}

static bool is_moving(const mb_wipe_assign_pass *pass, int x, int y)
{
    float luma[4];
    for (int k = 0; k < 4; k++) {
        luma[k] = wipe_luma(pass->frames[k], x, y);
    }
    float noise = (float)MB_WIPE_NOISE;
    return fabsf(luma[WIPE_AFTER] - luma[WIPE_TO]) > noise || fabsf(luma[WIPE_FROM] - luma[WIPE_BEFORE]) > noise;
}

static bool is_swept(sweep s, int x, int y)
{
    vec2 p = v2((float)x + 0.5f, (float)y + 0.5f);
    float from = line_at(s.from, lateral_of(p, s.axis));
    float to = line_at(s.to, lateral_of(p, s.axis));
    float along = along_of(p, s.axis);
    return along > fminf(from, to) + (float)MB_WIPE_EDGE && along < fmaxf(from, to) - (float)MB_WIPE_EDGE;
}

static bool is_still_content(const mb_wipe_assign_pass *pass, ivec2 cell, sweep s)
{
    float swept = 0.0f;
    float moving = 0.0f;
    for (int y = (cell.y - 1) * CELL; y < (cell.y + 2) * CELL; y++) {
        for (int x = (cell.x - 1) * CELL; x < (cell.x + 2) * CELL; x++) {
            if (wipe_texel_inside(pass->grid, x, y) && is_swept(s, x, y)) {
                swept += 1.0f;
                moving += is_moving(pass, x, y) ? 1.0f : 0.0f;
            }
        }
    }
    return swept >= 1.0f && moving <= (float)MB_WIPE_MAX_MOVING * swept;
}

static bool is_still_beyond(const mb_wipe_assign_pass *pass, ivec2 cell, sweep s, front_line line, int outward)
{
    int first = ((s.axis == 0 ? cell.y : cell.x) - 1) * CELL;
    float samples = 0.0f;
    float changed = 0.0f;
    for (int lateral = first; lateral < first + 3 * CELL; lateral++) {
        float front = line_at(line, (float)lateral + 0.5f);
        for (int k = 0; k < MB_WIPE_FRINGE; k++) {
            int along = (int)floorf(front + (float)outward * (1.0f + (float)k));
            ivec2 q = s.axis == 0 ? iv2(along, lateral) : iv2(lateral, along);
            if (wipe_texel_inside(pass->grid, q.x, q.y)) {
                float change = fabsf(wipe_luma(pass->frames[WIPE_TO], q.x, q.y) - wipe_luma(pass->frames[WIPE_FROM], q.x, q.y));
                samples += 1.0f;
                changed += change > (float)MB_WIPE_STILL ? 1.0f : 0.0f;
            }
        }
    }
    return changed <= (float)MB_WIPE_MAX_FRINGE * samples;
}

static bool is_sharp(const mb_wipe_assign_pass *pass, ivec2 cell, sweep s)
{
    return is_still_beyond(pass, cell, s, s.from, -s.direction) && is_still_beyond(pass, cell, s, s.to, s.direction);
}

static bool is_between(sweep s, ivec2 cell)
{
    vec2 centre = v2(((float)cell.x + 0.5f) * (float)CELL, ((float)cell.y + 0.5f) * (float)CELL);
    float from = line_at(s.from, lateral_of(centre, s.axis));
    float to = line_at(s.to, lateral_of(centre, s.axis));
    return along_of(centre, s.axis) >= fminf(from, to) && along_of(centre, s.axis) <= fmaxf(from, to);
}

static bool has_switched(const mb_wipe_assign_pass *pass, ivec2 cell, sweep s)
{
    float texels = 0.0f;
    float switched = 0.0f;
    for (int lateral = -MB_WIPE_LATERAL; lateral <= MB_WIPE_LATERAL; lateral++) {
        for (int along = -1; along <= 1; along++) {
            ivec2 c = iv2_add(cell, cell_step(s.axis, along, lateral));
            if (wipe_cell_inside(pass->grid, c.x, c.y) && is_between(s, c)) {
                texels += (float)(CELL * CELL);
                switched += seed_at(pass, WIPE_CHANGES, c).y;
            }
        }
    }
    return switched >= (float)MB_WIPE_MIN_CHANGED * texels;
}

static bool near_change(const mb_wipe_assign_pass *pass, ivec2 cell)
{
    for (int k = 0; k < 9; k++) {
        ivec2 c = iv2_add(cell, neighbour3(k));
        if (wipe_cell_inside(pass->grid, c.x, c.y) && seed_at(pass, WIPE_CHANGES, c).y > 0.0f) {
            return true;
        }
    }
    return false;
}

static void assign_cell(const mb_wipe_assign_pass *pass, int index)
{
    ivec2 cell = iv2(index % pass->grid.cells_x, index / pass->grid.cells_x);
    vec4 *fronts = vec4s(pass->fronts) + MB_WIPE_FRONT_VALUES * index;
    fronts[0] = (vec4){0.0f, 0.0f, 0.0f, 0.0f};
    fronts[1] = (vec4){0.0f, 0.0f, 0.0f, 0.0f};
    if (!near_change(pass, cell)) {
        return;
    }
    sweep s = shortest_sweep(pass, cell);
    if (s.span <= 0.0f || !has_switched(pass, cell, s) || !is_sharp(pass, cell, s) || !is_still_content(pass, cell, s)) {
        return;
    }
    fronts[0] = (vec4){s.from.offset, s.from.slope, s.to.offset, s.to.slope};
    fronts[1] = (vec4){(float)(s.axis + 1), 0.0f, 0.0f, 0.0f};
    words(pass->gate)[MB_WIPE_GATE_CELLS] += 1u;
}

void ref_wipe_assign(const mb_wipe_assign_pass *pass)
{
    if (!wipe_gate_open(pass->gate, MB_WIPE_GATE_SEEDS, 1u)) {
        return;
    }
    for (int index = 0; index < pass->grid.cells_x * pass->grid.cells_y; index++) {
        assign_cell(pass, index);
    }
}
