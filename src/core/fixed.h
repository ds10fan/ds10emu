/* Fixed-point helpers mirroring the DS-10 engine's arithmetic.
 *
 * The engine has no floating point available (ARM946E-S has no FPU), so every
 * value is an integer in a fixed Q format. This header exists so the exact
 * shift-and-truncate behaviour of the original ARM code is written down once
 * rather than open-coded, because bit-exactness depends on it.
 *
 * NEVER introduce float or double into src/core. The whole verification
 * strategy rests on integer determinism.
 */
#ifndef DS10_FIXED_H
#define DS10_FIXED_H

#include <stdint.h>

/* The audio signal format inside the voice: Q24, i.e. 1.0 == 0x01000000.
 * Every integrator in the filter clamps to +-(2^24 - 1). */
#define DS10_Q24       24
#define DS10_ONE_Q24   0x01000000
#define DS10_MAX_Q24   0x00FFFFFF

/* Clamp to the engine's signal range.
 *
 * Reproduces the original's asymmetric test exactly:
 *     if (v >= 0x01000000) v =  0x00FFFFFF;
 *     else if (v < -0x00FFFFFF) v = -0x00FFFFFF;
 * Note the upper bound compares against 0x01000000 but stores 0x00FFFFFF. */
static inline int32_t ds10_clamp24(int32_t v)
{
    if (v >= DS10_ONE_Q24)
        return DS10_MAX_Q24;
    if (v < -DS10_MAX_Q24)
        return -DS10_MAX_Q24;
    return v;
}

/* 64-bit multiply, result shifted right by 23.
 *
 * The original is SMULL followed by `lsr rlo,#23 ; orr rlo, rhi lsl #9`, which
 * is an arithmetic shift of the full 64-bit product. Doing this in 32 bits
 * would lose the high word. */
static inline int32_t ds10_mul23(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 23);
}

/* 64-bit multiply, result shifted right by 24 (the >>0x18 idiom). */
static inline int32_t ds10_mul24(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 24);
}

/* 64-bit multiply, result shifted right by 15 (the >>0xf idiom). */
static inline int32_t ds10_mul15(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 15);
}

/* 32-bit truncating multiply, wrapping on overflow.
 *
 * ARM `mul` keeps only the low 32 bits, and the engine relies on that in at
 * least one place (filter input * resonance). Signed overflow is undefined in
 * C, so the arithmetic is done through unsigned and cast back. */
static inline int32_t ds10_mul32(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a * (uint32_t)b);
}

/* Logical left shift of a signed value without relying on signed-overflow UB. */
static inline int32_t ds10_shl(int32_t v, unsigned n)
{
    return (int32_t)((uint32_t)v << n);
}

#endif /* DS10_FIXED_H */
