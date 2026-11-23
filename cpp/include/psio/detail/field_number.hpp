#pragma once
//
// psio/detail/field_number.hpp — the one field number of a record member.
//
// `field_number_v<T, I>` is the number `attr(member, field<N>)` gives member
// I of T, else its 1-based declaration position. Every format that
// identifies members by number reads this one value:
//
//   protobuf      the field number itself
//   Cap'n Proto   ordinals are assigned in ascending order of it
//   FlatBuffers   table field ids are assigned in ascending order of it
//
// so a record evolves the same way in all three: a new member takes a
// number above every number in use, and members keep their numbers.

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace psio::detail {

   template <typename T, std::size_t I>
   inline constexpr std::uint32_t field_number_v = [] {
      using R          = ::psio::reflect<T>;
      using FieldT     = typename R::template member_type<I>;
      constexpr auto P = R::template member_pointer<I>;
      constexpr auto found = ::psio::find_spec<::psio::field_num_spec>(
         ::psio::effective_annotations_for_v<T, FieldT, P>);
      if constexpr (found.has_value())
         return static_cast<std::uint32_t>(found->value);
      else
         return static_cast<std::uint32_t>(R::template field_number<I>);
   }();

   // The members of T in ascending order of field number (declaration
   // order among equal numbers): member index by rank.
   template <typename T>
   inline constexpr auto members_by_field_number = [] {
      constexpr std::size_t N = ::psio::reflect<T>::member_count;
      std::array<std::size_t, N>   order{};
      std::array<std::uint32_t, N> number{};
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         ((number[Is] = field_number_v<T, Is>, order[Is] = Is), ...);
      }(std::make_index_sequence<N>{});
      for (std::size_t i = 1; i < N; ++i)
         for (std::size_t j = i; j > 0 && number[order[j]] < number[order[j - 1]]; --j)
         {
            const auto t = order[j];
            order[j]     = order[j - 1];
            order[j - 1] = t;
         }
      return order;
   }();

}  // namespace psio::detail
