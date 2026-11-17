#include "motionblade/layout.h"

int layer_cell_index(ivec2 cell, ivec2 cells)
{
    cell = clamp(cell, ivec2(0), cells - 1);
    return cell.y * cells.x + cell.x;
}

#define LAYER_FIELD_AT(NAME, FIELD)                                                              \
    vec4 NAME(vec2 top_position, ivec2 cells)                                                    \
    {                                                                                            \
        vec2 g = clamp((top_position - 0.5 * float(MB_LAYER_CELL)) / float(MB_LAYER_CELL), vec2(0.0), \
                       vec2(cells - 1));                                                         \
        ivec2 c0 = ivec2(floor(g));                                                              \
        vec2 f = g - vec2(c0);                                                                   \
        vec4 top = mix(FIELD[layer_cell_index(c0, cells)], FIELD[layer_cell_index(c0 + ivec2(1, 0), cells)], f.x); \
        vec4 bottom = mix(FIELD[layer_cell_index(c0 + ivec2(0, 1), cells)],                      \
                          FIELD[layer_cell_index(c0 + ivec2(1, 1), cells)], f.x);                \
        return mix(top, bottom, f.y);                                                            \
    }

#define LAYER_KERNEL(NAME, SIGMA, RADIUS)                              \
    float NAME[2 * RADIUS + 1];                                        \
    void fill_##NAME()                                                 \
    {                                                                  \
        float sum = 0.0;                                               \
        for (int i = -RADIUS; i <= RADIUS; i++) {                      \
            NAME[i + RADIUS] = exp(-float(i * i) / (2.0 * SIGMA * SIGMA)); \
            sum += NAME[i + RADIUS];                                   \
        }                                                              \
        for (int i = 0; i <= 2 * RADIUS; i++) {                        \
            NAME[i] /= sum;                                            \
        }                                                              \
    }

#define LAYER_GATE(FIRST, SECOND, TRIGGER, BITS)                                                    \
    bool layer_moving()                                                                             \
    {                                                                                               \
        return ((BITS & 1) != 0 && FIRST[0].z > 0.5) || ((BITS & 2) != 0 && SECOND[0].z > 0.5);   \
    }                                                                                               \
    bool layer_gate_open()                                                                          \
    {                                                                                               \
        return layer_moving() && ((BITS & 4) == 0 || TRIGGER[0].z > float(MB_LAYER_ADOPTED));      \
    }
