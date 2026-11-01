#include "motionblade/half.h"

#include <math.h>
#include <string.h>

uint16_t mb_half_from_float(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000u;
    int exponent = (int)((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent >= 31) {
        return (uint16_t)(sign | 0x7c00u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return (uint16_t)sign;
        }
        mantissa |= 0x800000u;
        int shift = 14 - exponent;
        uint32_t half_mantissa = mantissa >> shift;
        uint32_t rest = mantissa & ((1u << shift) - 1);
        uint32_t halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half_mantissa & 1u))) {
            half_mantissa++;
        }
        return (uint16_t)(sign | half_mantissa);
    }
    uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
    uint32_t rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) {
        half++;
    }
    return (uint16_t)half;
}

float mb_half_to_float(uint16_t half)
{
    int exponent = (half >> 10) & 0x1f;
    int mantissa = half & 0x3ff;
    float magnitude = exponent == 0 ? ldexpf((float)mantissa, -24) : ldexpf((float)(mantissa | 0x400), exponent - 25);
    if (exponent == 31) {
        magnitude = INFINITY;
    }
    return (half & 0x8000u) ? -magnitude : magnitude;
}
