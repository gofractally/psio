/*============================================================================

This file is a minimal subset of Berkeley SoftFloat Release 3e by John R.
Hauser, providing only `psio_softfloat_f128_to_f64` (binary128 → binary64
narrowing conversion).

Upstream: https://github.com/ucb-bar/berkeley-softfloat-3
The full upstream license is in this directory's LICENSE.txt; per
clause 1, the SoftFloat copyright notice, conditions, and disclaimer
follow this paragraph.

------------------------------------------------------------------------------
Modifications relative to upstream (psio, 2026):
  * Single-file header; no platform.h, internals.h, primitives.h,
    specialize.h, or arch-specific overlays. The function body is
    unchanged from upstream `f128_to_f64.c`; the helpers are inlined
    verbatim from `include/primitives.h`. The rounding helper
    `softfloat_roundPackToF64` is reduced to its round-to-nearest-
    even (RNE) path with exception-flag tracking removed — psio always
    rounds RNE and does not surface IEEE-754 exception flags.
  * NaN handling produces a quiet binary64 NaN with the source sign
    preserved; payload bits are not propagated. This matches psio's
    canonical-NaN policy for binary16 widening (`pjson.hpp`,
    `f16_bits_to_double`).
  * `softfloat_detectTininess` and `softfloat_raiseFlags` references
    are removed. Underflow and overflow still produce the correct
    output magnitude (subnormal or ±inf); only the side-effect flag
    register is gone.

The verbatim 80-line `f128_to_f64` body below corresponds to upstream
`source/f128_to_f64.c` (Copyright 2011-2015 Regents of the UC).

==============================================================================

License for Berkeley SoftFloat Release 3e

John R. Hauser
2018 January 20

Copyright 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018 The Regents of the
University of California.  All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
    this list of conditions, and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions, and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

 3. Neither the name of the University nor the names of its contributors
    may be used to endorse or promote products derived from this software
    without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS "AS IS", AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ARE
DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

============================================================================*/

#ifndef PSIO_SOFTFLOAT_H
#define PSIO_SOFTFLOAT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert a 128-bit IEEE-754 binary128 value (16 little-endian bytes) to
 * a binary64 double, rounding to nearest-even. The input span must be
 * exactly 16 bytes. The result is a binary64 value:
 *   - finite-in-range inputs land within ±DBL_MAX after RNE rounding;
 *   - magnitudes above the binary64 range round to ±inf;
 *   - magnitudes below 2^-1074 round to 0 (signed);
 *   - subnormal inputs and outputs are handled per IEEE-754;
 *   - NaN inputs return a canonical quiet binary64 NaN with the source
 *     sign preserved (payload bits are not propagated).
 */
double psio_softfloat_f128_to_f64(const uint8_t bytes[16]);

#ifdef __cplusplus
}  /* extern "C" */
#endif

/*============================================================================
 * Implementation. Single-TU: compile this header into exactly one .c/.cpp
 * unit by defining PSIO_SOFTFLOAT_IMPL before including. (Other includers
 * see only the prototype.)
 *==========================================================================*/
#ifdef PSIO_SOFTFLOAT_IMPL

/* ── Type / macro shims (from softfloat_types.h, internals.h) ─────────── */

/* uint128 is the only multi-word primitive needed by f128_to_f64. */
struct psio_sf_uint128 { uint64_t v0, v64; };

/* Bit-field accessors on the upper 64 bits of an f128 representation,
 * lifted verbatim from internals.h. f128 is laid out as [v64=high, v0=low]
 * with sign:1 | exp:15 | frac64:48 packed in v64 and frac0:64 = v0. */
#define PSIO_SF_signF128UI64(a64) ((bool)((uint64_t)(a64) >> 63))
#define PSIO_SF_expF128UI64(a64)  ((int_fast32_t)((a64) >> 48) & 0x7FFF)
#define PSIO_SF_fracF128UI64(a64) ((a64) & UINT64_C(0x0000FFFFFFFFFFFF))
#define PSIO_SF_packToF64UI(sign, exp, sig)                          \
    ((uint64_t)(((uint_fast64_t)(sign) << 63)                        \
              + ((uint_fast64_t)(exp)  << 52)                        \
              + (sig)))

/* ── Primitives (verbatim from include/primitives.h) ──────────────────── */

/* Shifts the 128 bits formed by concatenating a64:a0 left by `dist`,
 * which must be in 1..63. */
static inline struct psio_sf_uint128
psio_sf_shortShiftLeft128(uint64_t a64, uint64_t a0, uint_fast8_t dist)
{
    struct psio_sf_uint128 z;
    z.v64 = (a64 << dist) | (a0 >> (-dist & 63));
    z.v0  = a0 << dist;
    return z;
}

/* Shifts `a` right by `dist` (which must not be zero). Any nonzero bits
 * shifted off are jammed into the LSB of the result. `dist` may exceed
 * 64; in that case the result is 0 or 1 by whether `a` was zero. */
static inline uint64_t
psio_sf_shiftRightJam64(uint64_t a, uint_fast32_t dist)
{
    return (dist < 63)
        ? (a >> dist) | ((uint64_t)(a << (-dist & 63)) != 0)
        : (a != 0);
}

/* ── Rounding (reduced subset of s_roundPackToF64.c) ──────────────────── */

/* RNE-only roundPackToF64. Upstream supports five rounding modes plus
 * exception-flag side-effects; psio uses RNE always and tracks no flags,
 * so this is the upstream function with everything else stripped. The
 * arithmetic is unchanged: the roundIncrement, the (sig+inc)>>10 step,
 * and the tie-to-even mask `! (roundBits ^ 0x200) & roundNearEven`. */
static inline double
psio_sf_roundPackToF64(bool sign, int_fast16_t exp, uint_fast64_t sig)
{
    const uint_fast16_t roundIncrement = 0x200;       /* RNE: half-LSB */
    uint_fast16_t       roundBits;
    uint_fast64_t       uiZ;
    double              z;

    if (0x7FD <= (uint16_t)exp) {
        if (exp < 0) {
            /* Subnormal output: shift sig right to align, accumulating
             * any lost bits into the round/sticky region. */
            sig = psio_sf_shiftRightJam64(sig, (uint_fast32_t)(-exp));
            exp = 0;
        } else if ((0x7FD < exp)
                || (UINT64_C(0x8000000000000000) <= sig + roundIncrement)) {
            /* Overflow → ±inf. The `- !roundIncrement` term in upstream
             * fires only for round-toward-zero; with RNE it's always 0. */
            uiZ = PSIO_SF_packToF64UI(sign, 0x7FF, 0);
            memcpy(&z, &uiZ, sizeof(z));
            return z;
        }
    }

    roundBits = sig & 0x3FF;
    sig = (sig + roundIncrement) >> 10;
    /* Tie-to-even: when roundBits is exactly 0x200, clear the LSB. */
    sig &= ~(uint_fast64_t)(!(roundBits ^ 0x200));
    if (!sig) exp = 0;

    uiZ = PSIO_SF_packToF64UI(sign, exp, sig);
    memcpy(&z, &uiZ, sizeof(z));
    return z;
}

/* ── Public entry point ───────────────────────────────────────────────── */

/* Body lifted verbatim from upstream f128_to_f64.c, with NaN-payload
 * propagation replaced by a sign-preserving canonical quiet NaN to match
 * psio's binary16 widen policy. */
double psio_softfloat_f128_to_f64(const uint8_t bytes[16])
{
    /* Little-endian 16-byte → (v0=low, v64=high) load. */
    uint64_t uiA0  = ((uint64_t)bytes[0])
                   | ((uint64_t)bytes[1]  << 8)
                   | ((uint64_t)bytes[2]  << 16)
                   | ((uint64_t)bytes[3]  << 24)
                   | ((uint64_t)bytes[4]  << 32)
                   | ((uint64_t)bytes[5]  << 40)
                   | ((uint64_t)bytes[6]  << 48)
                   | ((uint64_t)bytes[7]  << 56);
    uint64_t uiA64 = ((uint64_t)bytes[8])
                   | ((uint64_t)bytes[9]  << 8)
                   | ((uint64_t)bytes[10] << 16)
                   | ((uint64_t)bytes[11] << 24)
                   | ((uint64_t)bytes[12] << 32)
                   | ((uint64_t)bytes[13] << 40)
                   | ((uint64_t)bytes[14] << 48)
                   | ((uint64_t)bytes[15] << 56);

    bool          sign   = PSIO_SF_signF128UI64(uiA64);
    int_fast32_t  exp    = PSIO_SF_expF128UI64(uiA64);
    uint_fast64_t frac64 = PSIO_SF_fracF128UI64(uiA64);
    uint_fast64_t frac0  = uiA0;
    uint_fast64_t uiZ;
    double        z;
    struct psio_sf_uint128 frac128;

    if (exp == 0x7FFF) {
        if (frac64 | frac0) {
            /* NaN — produce canonical quiet f64 NaN, sign-preserved. */
            uiZ = ((uint64_t)sign << 63) | UINT64_C(0x7FF8000000000000);
        } else {
            uiZ = PSIO_SF_packToF64UI(sign, 0x7FF, 0);  /* ±inf */
        }
        memcpy(&z, &uiZ, sizeof(z));
        return z;
    }

    frac128 = psio_sf_shortShiftLeft128(frac64, frac0, 14);
    frac64  = frac128.v64 | (frac128.v0 != 0);
    if (!(exp | frac64)) {
        uiZ = PSIO_SF_packToF64UI(sign, 0, 0);
        memcpy(&z, &uiZ, sizeof(z));
        return z;
    }

    exp -= 0x3C01;
    return psio_sf_roundPackToF64(sign, (int_fast16_t)exp,
                                  frac64 | UINT64_C(0x4000000000000000));
}

#endif  /* PSIO_SOFTFLOAT_IMPL */

#endif  /* PSIO_SOFTFLOAT_H */
