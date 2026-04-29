#pragma once
//
// libraries/psio/cpp/include/psio/varint/detail/cpu.hpp
//
// Compile-time CPU-feature gates used by the varint fast paths. The
// `varint::<algo>::fast` namespace is implemented in terms of these:
// when the macro for a relevant feature is set, the fast path uses the
// SIMD/bit-manipulation instructions; otherwise it aliases to the
// portable scalar path.
//
// We intentionally do NOT do runtime CPU dispatch — psio callers are
// compiled with a known target triple (Apple Silicon for this tree;
// `-march=native` or an equivalent for x86 hosts). Adding runtime
// dispatch later is non-breaking because the public API is the same
// free-function shape regardless.
//

namespace psio1::varint::detail {

#if defined(__BMI2__)
   inline constexpr bool has_bmi2 = true;
#else
   inline constexpr bool has_bmi2 = false;
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
   inline constexpr bool has_neon = true;
#else
   inline constexpr bool has_neon = false;
#endif

}  // namespace psio1::varint::detail
