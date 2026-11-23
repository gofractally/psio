#pragma once
//
// psio/untagged.hpp — marking a variant as untagged in the text formats.
//
// A std::variant is written to JSON with its alternative named
// (`{"string":"x"}` under ijson, `[0,"x"]` under json). An UNTAGGED variant
// is written as the bare value of its alternative — `"x"`, `["a","b"]` —
// and a reader tells the alternatives apart by the kind of JSON value each
// is written as (docs/json.md "Untagged variants"). It is a rule of the
// text formats only: in PSSZ and every binary format the variant has its
// selector as always.
//
// Opt in by declaring, for the variant type, before it is first written or
// read:
//
//     using Extra = std::variant<std::string, std::vector<std::string>>;
//     namespace psio { constexpr bool psio_is_untagged(const Extra*) { return true; } }
//
// (in namespace psio, or in the namespace of the variant type or of one of
// its alternatives: the hook is found by argument-dependent lookup).
// `psio::is_untagged_v<T>` is the answer the codecs use.

namespace psio {

   // The default: tagged. (Kept for callers that ask with a pointer.)
   template <typename T>
   constexpr bool psio_is_untagged(const T*)
   {
      return false;
   }

   // An argument that converts to `const T*` and whose namespace is psio,
   // so that a hook declared in namespace psio AFTER this header (for a
   // std::variant, which has no namespace of its own to put it in) is
   // found, by argument-dependent lookup, when is_untagged_v<T> is first
   // needed.
   template <typename T>
   struct untagged_probe
   {
      constexpr operator const T*() const noexcept { return nullptr; }
   };
   // No hook for T: tagged.
   constexpr bool psio_is_untagged(...) { return false; }

   template <typename T>
   inline constexpr bool is_untagged_v = psio_is_untagged(untagged_probe<T>{});

}  // namespace psio
