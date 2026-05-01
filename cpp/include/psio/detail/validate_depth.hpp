#pragma once
//
// psio/detail/validate_depth.hpp — shared depth limit for `validate()`.
//
// Validation is the untrusted-input path. A 64-level hard cap on
// recursion depth shuts down stack-exhaustion attack vectors that a
// soft "recommendation" cannot. Decoders MAY accept deeper trust at
// their own risk; structural validators MUST refuse.
//
// Every format's structural validator threads a `depth` counter into
// its recursive walker, increments at each container/record level,
// and returns `codec_fail("max depth exceeded", ...)` when the count
// exceeds `kMaxValidationDepth`.
//
// Rationale (matches docs/pssz-spec.md §8 — the "hard requirement"
// promotion): a malicious buffer can claim arbitrarily nested
// records / lists / variants / optionals; without a cap the walker
// blows the C stack long before the buffer is exhausted. The cap
// lives at a depth that real-world schemas never reach (Cap'n Proto
// uses 64 by default for the same reason).

#include <cstddef>

namespace psio {

   // Hard cap on recursion depth for psio::validate<T>(...).
   //
   // Exposed as an inline constexpr so all format walkers share the
   // same constant without ODR risk and so the spec-doc value stays
   // grep-able from a single header.
   inline constexpr std::size_t kMaxValidationDepth = 64;

}  // namespace psio
