/* Decompiled widget functions. See widget.h for what belongs here. */
#include "widget.h"

#include "../core/tables_data.h"

/* 0x02055b98's clamp, which is the order the original writes it in: above the
 * maximum first, then below the minimum, so a min above the max yields the
 * min. */
int32_t ds10_widget_clamp(int min, int max, int value)
{
    if (max < value) return max;
    if (value < min) return min;
    return value;
}

/* half = (sweep / 2) * 0x10000 / 360.
 *
 * The original does not divide: it multiplies by 0xB60B60B7 and takes the high
 * word, adds the operand back, shifts right by 8 and corrects the sign - the
 * compiler's expansion of a signed divide by 360. That is exact for every
 * value it can be given here, so this divides.
 *
 * `sweep / 2` truncates toward zero first, which is the original's own
 * `param_5 / 2` and matters for an odd sweep.
 */
int32_t ds10_widget_knob_half(int sweep)
{
    int32_t x = (int32_t)(sweep / 2) * 0x10000;

    return x / 360;
}

int16_t ds10_widget_knob_angle(int min, int max, int value, int sweep,
                               int reverse)
{
    int32_t half = ds10_widget_knob_half(sweep);
    int32_t start = 0x10000 - half;
    int32_t span = half * 2;
    int32_t range = max - min;
    int32_t d = reverse ? range - (value - min) : value - min;
    int32_t term;

    /* The divide helper returns zero for a zero denominator rather than
     * faulting, which is what a knob whose minimum equals its maximum gets. */
    term = range ? span * d / range : 0;
    return (int16_t)(start + term);
}

void ds10_widget_affine(int16_t angle, int32_t sx, int32_t sy, int32_t *m)
{
    int idx = ((uint16_t)angle >> 4) & 0xFFF;
    int32_t sn = ds10_tbl_sincos[idx * 2];
    int32_t cs = ds10_tbl_sincos[idx * 2 + 1];

    /* 0x02011fc4 lays the rotation out in this order, and 0x02011fd0 scales
     * the first pair by sx and the second by sy. */
    m[0] = (int32_t)(((int64_t)sx * cs) >> 12);
    m[1] = (int32_t)(((int64_t)sx * sn) >> 12);
    m[2] = (int32_t)(((int64_t)sy * -sn) >> 12);
    m[3] = (int32_t)(((int64_t)sy * cs) >> 12);
}
