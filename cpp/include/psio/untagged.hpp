#pragma once
//
// psio/untagged.hpp — ADL hook for marking variants as untagged.
//
// Variants whose alternatives have unambiguously distinct shapes can
// be encoded without a discriminator tag — receivers infer the
// alternative from the value's wire layout alone.  Specialise
// `psio_is_untagged(const MyVariant*)` to opt in:
//
//     constexpr bool psio_is_untagged(const MyVariant*) { return true; }
//
// Format codecs consult this hook during variant encode/decode.
// (Codec wiring is a follow-up — the hook is registered here so call
// sites can mark types now.)

namespace psio {

   template <typename T>
   constexpr bool psio_is_untagged(const T*)
   {
      return false;
   }

}  // namespace psio
