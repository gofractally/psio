# softfloat — vendored subset

Minimal extraction from **Berkeley SoftFloat Release 3e** (John R. Hauser,
UC Berkeley) providing one function: `psio_softfloat_f128_to_f64` —
IEEE-754 binary128 → binary64 narrowing conversion with round-to-nearest-
even.

Upstream: <https://github.com/ucb-bar/berkeley-softfloat-3>
License: 3-clause BSD; see `LICENSE.txt` (verbatim copy of upstream
`COPYING.txt`).

## Why we vendor instead of depend

We need exactly one function (`f128_to_f64`) for pjson decode of
binary128 (`§4.6` of `docs/pjson-spec.md`). Pulling SoftFloat as a
submodule or external dependency would add ~6k LoC of build
infrastructure for a single ~200-line code path. Vendoring the
relevant subset keeps the build hermetic and the code review
auditable.

## What's included

Single header at `psio_softfloat.h`. Two-step include:

```cpp
#include <psio_softfloat.h>          // declarations only

// In exactly one translation unit:
#define PSIO_SOFTFLOAT_IMPL
#include <psio_softfloat.h>          // definitions
```

## What's modified relative to upstream

The `f128_to_f64` body is verbatim from
`source/f128_to_f64.c`. The supporting code is reduced:

* **Single rounding mode (RNE).** Upstream supports five IEEE-754 rounding
  modes selected by the `softfloat_roundingMode` global. psio always uses
  round-to-nearest-even, so `psio_sf_roundPackToF64` keeps only the RNE
  arithmetic. The `roundIncrement = 0x200`, `(sig + inc) >> 10`, and
  tie-to-even mask `~(uint64_t)(!(roundBits ^ 0x200))` are all unchanged.

* **No exception flags.** Upstream sets `softfloat_exceptionFlags`
  (underflow, overflow, inexact). psio doesn't expose IEEE-754 flags, so
  these calls are removed. The numeric output is unchanged — overflow
  still produces ±inf, subnormals are still rounded correctly.

* **No NaN payload propagation.** Upstream propagates source NaN payload
  bits (truncated to f64 width) per architecture-specific rules in
  `source/<arch>/specialize.h`. psio's binary16 widen
  (`pjson.hpp::f16_bits_to_double`) already canonicalizes NaN to a quiet
  NaN with sign preserved; this matches that policy for consistency
  across the binary{16,128} → 64 paths.

* **No `softfloat_detectTininess` configuration.** Upstream lets a global
  pick whether tininess is detected before or after rounding (only matters
  for the underflow flag, which we don't track).

* **Inputs are 16 little-endian bytes.** Upstream takes a packed
  `float128_t` (a struct of two `uint64_t`); psio's wire format is a raw
  16-byte little-endian sequence (`pjson-spec.md §4.6`), so the entry
  point loads bytes directly. No host-endian assumptions.

The verbatim 80-line `f128_to_f64` body retains the upstream copyright
notice in the header file's lead block, satisfying clause 1 of the
3-clause BSD license.

## Cross-validation

The Rust port at `rust/psio/src/pjson_json.rs` mirrors this exact
algorithm and is cross-checked against the C++ result on a shared
corpus (zeros, ±inf, qNaN, sNaN, ±MAX, ±MIN_NORMAL, ±MIN_SUBNORMAL,
a handful of normals at varying exponents). See `cpp/tests/pjson_tests.cpp`
and `rust/psio/src/pjson_cross_validation_tests.rs`.
