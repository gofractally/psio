#pragma once
//
// psio/transparent.hpp — transparent wrapper types.
//
// A reflected struct with exactly one data member, declared transparent,
//
//     struct SubjectPublicKeyInfo {
//        std::vector<unsigned char> data;
//        PSIO_REFLECT(SubjectPublicKeyInfo, transparent(), data)
//     };
//
// (or `PSIO_TYPE_ATTRS(SubjectPublicKeyInfo, psio::transparent)`), IS its
// member in every core format: the same pssz bytes (fixed-ness, size, offset
// width, optional selector rule, bulk copies), the same key bytes, the same
// JSON / I-JSON text, the same view. It stays its own C++ type, with its own
// identity for a text adapter (PSIO_ADAPTER), an invariant
// (psio/invariant.hpp), its Schema IR node (a named alias of the member's
// type), get_type_name and the GraphQL type name.
//
//   psio::is_transparent_v<T>       T is declared transparent
//   psio::transparent_inner_t<T>    the member's type
//   psio::transparent_inner(v)      the member of a value
//
// Compile errors:
//   - not exactly one data member;
//   - combined with definitionWillNotChange / maxFields / maxDynamicData:
//     they describe a record layout, and a transparent type has none (a cap
//     or a fixed layout is a property of the member's type);
//   - an annotation on the member (attr(...) / PSIO_FIELD_ATTRS): the type
//     is its member wherever it appears — bare, as a list element, as an
//     optional payload — and a member annotation has no enclosing record to
//     act in. Bound the member's type instead (psio::bounded<T, N>,
//     psio::utf8_string<N>) or state the constraint as the type's invariant;
//   - the member is a std::optional: presence belongs to the record that
//     holds the value (its trailing absent members are pruned). Wrap the
//     payload and hold a std::optional<Wrapper>.
//
// A record that holds a member of a transparent type may give that member
// the annotations that reach a value through any type (the integer text
// form: json_string / ijson_number); a length_bound on it is a compile
// error, as on any member that is not itself a string or a list.
//
// Outside the core formats (pssz, key, json, ijson, the Schema IR and the
// schema-driven codecs) the type is what it is declared as, a struct of one
// member: bin / borsh / bincode encode that as the member anyway; ssz, frac
// and the foreign-schema formats see the struct.

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>

#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psio
{
   namespace detail
   {
      template <typename T>
      inline constexpr bool is_std_optional_type_v = false;
      template <typename T>
      inline constexpr bool is_std_optional_type_v<std::optional<T>> = true;

      // Whether T carries the declaration; the annotations of a type that
      // is not a reflected class are never read.
      template <typename T>
      constexpr bool transparent_declared() noexcept
      {
         if constexpr (std::is_class_v<T> && Reflected<T>)
            return has_spec_v<transparent_spec,
                              std::remove_cvref_t<decltype(annotate<type<T>{}>)>>;
         else
            return false;
      }

      // The rules a declaration must meet. Each one is its own
      // static_assert so the diagnostic names the reason.
      template <typename T>
      constexpr bool transparent_checked() noexcept
      {
         using R  = reflect<T>;
         using TT = std::remove_cvref_t<decltype(annotate<type<T>{}>)>;
         static_assert(R::member_count == 1,
                       "psio: a transparent() type has exactly one data member");
         static_assert(!has_spec_v<definition_will_not_change, TT> &&
                          !has_spec_v<max_fields_spec, TT> &&
                          !has_spec_v<max_dynamic_data_spec, TT>,
                       "psio: transparent() cannot be combined with "
                       "definitionWillNotChange / maxFields / maxDynamicData: they "
                       "describe a record layout, and a transparent type is its member");
         if constexpr (R::member_count == 1)
         {
            static_assert(
               std::tuple_size_v<std::remove_cvref_t<
                     decltype(annotate<R::template member_pointer<0>>)>> == 0,
               "psio: the member of a transparent() type takes no annotation "
               "(attr / PSIO_FIELD_ATTRS): bound the member's type or declare an "
               "invariant");
            static_assert(
               !is_std_optional_type_v<std::remove_cv_t<typename R::template member_type<0>>>,
               "psio: the member of a transparent() type is not a std::optional: "
               "wrap the payload and hold a std::optional of the wrapper");
         }
         return true;
      }
   }  // namespace detail

   template <typename T>
   inline constexpr bool is_transparent_v = [] {
      using U = std::remove_cvref_t<T>;
      if constexpr (detail::transparent_declared<U>())
         return detail::transparent_checked<U>();
      else
         return false;
   }();

   namespace detail
   {
      template <typename T, bool = is_transparent_v<T>>
      struct transparent_inner
      {
      };
      template <typename T>
      struct transparent_inner<T, true>
      {
         using type = std::remove_cv_t<typename reflect<T>::template member_type<0>>;
      };
   }  // namespace detail

   // The member's type; ill-formed (SFINAE-friendly) for other types.
   template <typename T>
   using transparent_inner_t = typename detail::transparent_inner<std::remove_cvref_t<T>>::type;

   // The member of a transparent value.
   template <typename T>
      requires is_transparent_v<T>
   constexpr decltype(auto) transparent_inner(T&& value) noexcept
   {
      return (std::forward<T>(value).*
              (reflect<std::remove_cvref_t<T>>::template member_pointer<0>));
   }

}  // namespace psio
