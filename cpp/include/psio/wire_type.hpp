#pragma once
//
// psio/wire_type.hpp — the type a value is encoded AS.
//
// Most values are encoded as their own type. A few C++ types only refer to
// a value that lives elsewhere, and are encoded as the type of what they
// refer to — the same bytes the owning type produces:
//
//   std::string_view, const char*, char[N]   std::string (a C string
//                                            without its terminating NUL)
//   std::span<E> / std::span<const E>        std::vector<E>
//   std::reference_wrapper<X>, X&            X
//   psio::view<X, Fmt>                       X   (view.hpp)
//   psio::buffer<X, pssz, Store>             psio::nested<X>   (pssz_view.hpp:
//                                            pssz_buffer<X>, shared_pssz<X>)
//
// and a container of such sources as the container of what they encode as:
//
//   std::tuple<Ss...> / std::pair<A, B>      std::tuple / std::pair of the
//                                            wire types (std::tie(a, b),
//                                            std::forward_as_tuple(...))
//   std::vector<S>, std::optional<S>         std::vector / std::optional of
//                                            the wire type
//
// `psio::wire_type_t<S>` names that type (S itself for everything else).
// It is what a schema, a validator, a reader and a method's argument tuple
// (`reflect<T>::method_arg_tuple<I>`) see for S; an encoder accepts S and
// writes the bytes of `wire_type_t<S>`.
//
// The trait is a customization point: specialize `psio::wire_type<S>` for a
// type of your own that packs as another (the bytes must be those of the
// type you name; the pssz encoder then needs to be able to walk it as that
// type — a string-like S needs data() / size(), a list-like S size() and
// iteration).

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio
{
   template <typename S>
   struct wire_type
   {
      using type = S;
   };
   template <typename S>
   using wire_type_t = typename wire_type<std::remove_cvref_t<S>>::type;

   // Whether S is encoded as a type other than itself.
   template <typename S>
   inline constexpr bool is_source_v =
      !std::is_same_v<std::remove_cvref_t<S>, wire_type_t<S>>;

   template <>
   struct wire_type<std::string_view>
   {
      using type = std::string;
   };
   template <>
   struct wire_type<const char*>
   {
      using type = std::string;
   };
   template <>
   struct wire_type<char*>
   {
      using type = std::string;
   };
   template <std::size_t N>
   struct wire_type<char[N]>
   {
      using type = std::string;
   };
   template <typename E, std::size_t N>
   struct wire_type<std::span<E, N>>
   {
      using type = std::vector<wire_type_t<E>>;
   };
   template <typename X>
   struct wire_type<std::reference_wrapper<X>>
   {
      using type = wire_type_t<X>;
   };
   namespace detail
   {
      // An element of a tuple / pair: a reference or a source becomes the
      // value type it is encoded as; anything else stays exactly as it is
      // declared (std::pair<const K, V> is not a source).
      template <typename E>
      using wire_element_t =
         std::conditional_t<std::is_reference_v<E> || is_source_v<E>, wire_type_t<E>, E>;
   }  // namespace detail
   template <typename... Ss>
   struct wire_type<std::tuple<Ss...>>
   {
      using type = std::tuple<detail::wire_element_t<Ss>...>;
   };
   template <typename A, typename B>
   struct wire_type<std::pair<A, B>>
   {
      using type = std::pair<detail::wire_element_t<A>, detail::wire_element_t<B>>;
   };
   template <typename S>
   struct wire_type<std::optional<S>>
   {
      using type = std::optional<wire_type_t<S>>;
   };
   // (A vector with a custom allocator is left as it is.)
   template <typename S>
      requires is_source_v<S>
   struct wire_type<std::vector<S>>
   {
      using type = std::vector<wire_type_t<S>>;
   };

   namespace detail
   {
      // A C string as its characters.
      inline std::string_view c_string_view(const char* s) noexcept
      {
         return s ? std::string_view{s} : std::string_view{};
      }
   }  // namespace detail

}  // namespace psio
