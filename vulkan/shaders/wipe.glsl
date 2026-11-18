float along_of(vec2 p, int axis)
{
    return axis == 0 ? p.x : p.y;
}

float lateral_of(vec2 p, int axis)
{
    return axis == 0 ? p.y : p.x;
}

ivec2 cell_step(int axis, int along, int lateral)
{
    return axis == 0 ? ivec2(along, lateral) : ivec2(lateral, along);
}

bool cell_inside(ivec2 c)
{
    return c.x >= 0 && c.y >= 0 && c.x < push.cells_x && c.y < push.cells_y;
}

vec4 seed_at(int kind, ivec2 c)
{
    return seeds[MB_WIPE_SEED_VALUES * (c.y * push.cells_x + c.x) + kind];
}

bool has_seed(int kind, ivec2 c, float code)
{
    return cell_inside(c) && seed_at(kind, c).z == code;
}

int lateral_offset(int k)
{
    return (k & 1) != 0 ? -(k + 1) / 2 : k / 2;
}

int wipe_slot(int kind, int axis, bool positive)
{
    return kind * 4 + axis * 2 + (positive ? 1 : 0);
}
