#pragma once
//
// psio/variant_label.hpp — the label of a variant alternative.
//
// An alternative of a variant has ONE label. It is the alternative's name
// everywhere a name is used: the key of the tagged text form
// (`{"Label": value}`, json.hpp / pssz_ir_json.hpp), the name of the
// alternative in the Schema IR (`Variant::members[i].name`,
// schema_builder.hpp), and what a schema comparison calls it. Labels are
// not on the wire of a binary format: an alternative is selected by its
// position there.
//
// The rule, for every language (docs/annotations.md "Variant alternative
// labels"):
//
//   1. the name the declaration gives the alternative, where the language
//      names alternatives (a Rust enum variant's identifier, the key of a
//      JavaScript `variant({...})`);
//   2. else the name of the alternative's type, as its language declares
//      it. A std::variant names no alternative, so in C++ this is
//      `get_type_name` (get_type_name.hpp): a reflected type's reflected
//      name, and for the types that have no declared name
//
//         bool int8 uint8 int16 uint16 int32 uint32 int64 uint64 int128
//         uint128 float32 double char string
//         T[]      std::vector<T>          T[N]     std::array<T, N>
//         T?       std::optional<T>        tuple_A_B   std::tuple / pair
//         variant_A_B                      map_K_V, set_K
//
//      built from the names of the types they hold. A type that only
//      states a bound is named as what it bounds: psio::utf8_string<N> is
//      `string`, psio::bounded<T, N> is `T[]`, psio::byte_array<N> is
//      `uint8[N]`;
//   3. else its position, "0", "1", ...: for a type with no name at all,
//      and for an alternative whose type name another alternative of the
//      same variant has too (`std::variant<std::uint32_t, std::string,
//      std::string>` is labelled "uint32", "1", "2"). A variant always has
//      labels, and they are always distinct.
//
// Note what rule 3 means for a type that evolves: appending a second
// alternative of a type the variant already holds relabels the first one
// (its text key changes; its bytes do not). A schema comparison reports it
// (`alternative-renamed`, pssz_ir_compare.hpp); stating the labels avoids
// it.
//
// To state them — to match a name imposed from outside, or to tell two
// alternatives of one type apart — specialize `psio::variant_labels` with
// one label per alternative, in order:
//
//    using Shape = std::variant<Circle, Square, std::uint32_t>;
//    template <> struct psio::variant_labels<Shape>
//    {
//       static constexpr std::array<std::string_view, 3> names{"circle", "square", "side"};
//    };

#include <psio/get_type_name.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <variant>

namespace psio
{
   // The wrapper types that state a bound in the type are named as what
   // they bound, so that an alternative written `psio::utf8_string<16>` and
   // one written `std::string` under `at<I>(utf8 | max<16>)` have one label.
   template <std::size_t N>
   constexpr const char* get_type_name(const utf8_string<N>*)
   {
      return "string";
   }
   template <typename T, std::size_t N>
   constexpr const char* get_type_name(const bounded<T, N>*)
   {
      return vector_type_name<T>.data();
   }
   template <std::size_t N>
   constexpr const char* get_type_name(const byte_array<N>*)
   {
      return array_type_name<std::uint8_t, N>.data();
   }

   // The stated labels of a variant type (see above). Unspecialized: every
   // alternative has its default label.
   template <typename Variant>
   struct variant_labels
   {
   };

   namespace detail
   {
      // The decimal text of an alternative's position.
      template <std::size_t I>
      struct variant_position_text
      {
         static constexpr auto value = [] {
            std::size_t digits = 1;
            for (std::size_t n = I; n >= 10; n /= 10)
               ++digits;
            std::array<char, 4> out{};
            std::size_t         n = I;
            for (std::size_t at = digits; at > 0; --at, n /= 10)
               out[at - 1] = static_cast<char>('0' + n % 10);
            return out;
         }();
         static constexpr std::size_t size = I >= 100 ? 3 : I >= 10 ? 2 : 1;
      };

      template <typename V>
      concept has_variant_labels = requires { ::psio::variant_labels<V>::names; };
   }  // namespace detail

   namespace detail
   {
      // Rule 2: the name of alternative I's type; empty when it has none.
      template <typename V, std::size_t I>
      constexpr std::string_view variant_type_label() noexcept
      {
         using A = std::variant_alternative_t<I, V>;
         if constexpr (requires { get_type_name(static_cast<const A*>(nullptr)); })
            return std::string_view{get_type_name(static_cast<const A*>(nullptr))};
         else
            return {};
      }
      // Whether rule 2 gives alternative I a label no other alternative has.
      template <typename V, std::size_t I>
      constexpr bool variant_type_label_unique() noexcept
      {
         const std::string_view mine = variant_type_label<V, I>();
         if (mine.empty())
            return false;
         return []<std::size_t... Js>(std::string_view name, std::index_sequence<Js...>) {
            return ((Js == I || variant_type_label<V, Js>() != name) && ...);
         }(mine, std::make_index_sequence<std::variant_size_v<V>>{});
      }
   }  // namespace detail

   // The label of alternative I of the variant type V.
   template <typename V, std::size_t I>
   constexpr std::string_view variant_label() noexcept
   {
      if constexpr (detail::has_variant_labels<V>)
      {
         static_assert(std::size(::psio::variant_labels<V>::names) == std::variant_size_v<V>,
                       "psio::variant_labels<V>::names has one label per alternative of V");
         return std::string_view{::psio::variant_labels<V>::names[I]};
      }
      else if constexpr (detail::variant_type_label_unique<V, I>())
         return detail::variant_type_label<V, I>();
      else
         return std::string_view{detail::variant_position_text<I>::value.data(),
                                 detail::variant_position_text<I>::size};
   }

   // The labels of V in order: a lookup table source (name_lookup.hpp).
   template <typename V>
   struct variant_label_table
   {
      static constexpr auto _names_array_ = []<std::size_t... Is>(std::index_sequence<Is...>) {
         return std::array<std::string_view, sizeof...(Is)>{variant_label<V, Is>()...};
      }(std::make_index_sequence<std::variant_size_v<V>>{});
   };

   // Whether the labels of V are nonempty and distinct (the default labels
   // always are; stated ones must be).
   template <typename V>
   constexpr bool variant_labels_ok() noexcept
   {
      const auto& names = variant_label_table<V>::_names_array_;
      for (std::size_t a = 0; a < names.size(); ++a)
      {
         if (names[a].empty())
            return false;
         for (std::size_t b = a + 1; b < names.size(); ++b)
            if (names[a] == names[b])
               return false;
      }
      return true;
   }
}  // namespace psio

// The message of the check every user of the labels makes.
#define PSIO_REQUIRE_VARIANT_LABELS(V)                                                      \
   static_assert(::psio::variant_labels_ok<V>(),                                            \
                 "psio: psio::variant_labels<V> states one nonempty label per alternative " \
                 "of V, and no label twice")
