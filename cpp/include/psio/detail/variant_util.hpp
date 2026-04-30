#pragma once
//
// psio/detail/variant_util.hpp — shared variant helpers for format
// codecs. Every binary format encodes std::variant as "index + payload";
// the index width and payload layout are format-specific, but the
// runtime-to-compile-time index dispatch is common and lives here.

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace psio::detail {

   template <typename T>
   struct is_std_variant : std::false_type {};
   template <typename... Ts>
   struct is_std_variant<std::variant<Ts...>> : std::true_type {};

   // Build a variant<Ts...> where the I-th alternative is decoded via
   // `decode_alt(std::in_place_index<I>)`. Fold expression short-circuits
   // on match so only one alternative is constructed.
   template <typename V, typename Fn, std::size_t... Is>
   constexpr V variant_from_index_impl(std::size_t idx, Fn&& decode_alt,
                                       std::index_sequence<Is...>)
   {
      V out;
      const bool found =
         ((idx == Is
              ? (out = decode_alt(std::in_place_index<Is>), true)
              : false) ||
          ...);
      (void)found;
      return out;
   }

   // Given a runtime variant index `idx` in [0, sizeof...(Ts)) and a
   // callable that decodes alternative I given `in_place_index<I>`,
   // construct the variant.
   template <typename... Ts, typename Fn>
   constexpr std::variant<Ts...>
   variant_from_index(std::size_t idx, Fn&& decode_alt)
   {
      return variant_from_index_impl<std::variant<Ts...>>(
         idx, std::forward<Fn>(decode_alt),
         std::index_sequence_for<Ts...>{});
   }

}  // namespace psio::detail
