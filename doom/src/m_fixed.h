//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Fixed point arithemtics, implementation.
//


#ifndef __M_FIXED__
#define __M_FIXED__




//
// Fixed point, 32bit as 16.16.
//
#ifndef FRACBITS
#define FRACBITS		16
#endif
#define FRACUNIT		(1<<FRACBITS)

typedef int fixed_t;

#if PICO_BUILD
#include <stdint.h>
static inline fixed_t FixedMulInline(fixed_t a, fixed_t b) {
#if PICO_ON_DEVICE
    uint32_t tmp1, tmp2, tmp3;
    __asm__ volatile (
    ".syntax unified\n"
    "asrs   %[r_tmp1], %[r_b], #16 \n" // r_tmp1 = BH
    "uxth   %[r_tmp2], %[r_a]      \n" // r_tmp2 = AL
    "muls   %[r_tmp2], %[r_tmp1]   \n" // r_tmp2 = BH * AL
    "asrs   %[r_tmp3], %[r_a], #16 \n" // r_tmp3 = AH
    "muls   %[r_tmp1], %[r_tmp3]   \n" // r_tmp1 = BH * AH
    "uxth   %[r_b], %[r_b]         \n" // r_b = BL
    "uxth   %[r_a], %[r_a]         \n" // r_a = AL
    "muls   %[r_a], %[r_b]         \n" // r_a = AL * BL
    "muls   %[r_b], %[r_tmp3]      \n" // r_b = BL * AH
    "add    %[r_b], %[r_tmp2]      \n" // r_b = BL * AH + BH * AL
    "lsls   %[r_tmp1], #16         \n" // r_tmp1 = (BH * AH) L << 16
    "lsrs   %[r_a], #16            \n" // r_a = (AL & BL) H
    "add    %[r_a], %[r_b]         \n"
    "add    %[r_a], %[r_tmp1]      \n"
    : [r_a] "+l" (a), [r_b] "+l" (b), [r_tmp1] "=&l" (tmp1), [r_tmp2] "=&l" (tmp2), [r_tmp3] "=&l" (tmp3)
    :
    );
    return a;
#else
    return (((uint64_t)a) * b) >> FRACBITS;
#endif
}
#endif

#if defined(__ARM_ARCH_8M_MAIN__) && !PICO_BUILD
/* Cortex-M33: GCC lowers the int64 multiply to a single SMULL; inlining
 * eliminates the BL/BX and lets the surrounding code share registers
 * with operands. Hot for pd_add_plane_column's per-pixel loop. */
#include <stdint.h>
static inline fixed_t FixedMul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}
#define FIXED_MUL_INLINED 1
#endif

#ifndef FIXED_MUL_INLINED
fixed_t FixedMul	(fixed_t a, fixed_t b);
#endif
fixed_t FixedDiv	(fixed_t a, fixed_t b);


#endif
