#pragma once
//
// psio/detail/tag_invoke.hpp — P1895 customization-point plumbing.
//
// A CPO (customization point object) is an `inline constexpr` function
// object. Each CPO in psio invokes `tag_invoke(cpo_object{}, ...)`
// via argument-dependent lookup; format authors supply the matching
// overload as hidden friends on their format tag struct.
//
// This header provides the `tag_invoke` name in psio so every CPO's
// `tag_invoke(*this, ...)` expression sees a qualified name that ADL
// can extend. The function itself is never defined — it only exists
// as a "handle" for ADL.

namespace psio {

   // Sentinel so `tag_invoke` is a reachable name for qualified ADL.
   // Users never call this directly; the CPOs do.
   //
   // Functions declared in this way are found by:
   //   1. Qualified lookup finds this declaration.
   //   2. The compiler then performs ADL on the arguments, which
   //      brings in any hidden-friend overloads on the format tag.
   //
   // If no overload matches, the call is ill-formed — the CPO's
   // `requires` clause catches this and produces a readable error
   // at the CPO call site rather than deep in an ADL failure.
   void tag_invoke();

}  // namespace psio
