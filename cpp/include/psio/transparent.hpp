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
//   - an annotation on the member that the Schema IR cannot state as a type
//     (see "Bounds on the member" below);
//   - the member is a std::optional: presence belongs to the record that
//     holds the value (its trailing absent members are pruned). Wrap the
//     payload and hold a std::optional<Wrapper>.
//
// Bounds on the member. The member may carry the annotations that make a
// plain type a bounded one:
//
//     struct Memo {
//        std::string text;
//        PSIO_REFLECT(Memo, transparent(), attr(text, utf8 | max<80>))
//     };
//
//   utf8                on a std::string: a UTF-8 string
//   max<N>              on a std::string, a std::vector, a map or a set
//   element / keys / values / at<I>(...)   the same below the member
//
// The wrapper then IS that bounded type wherever it appears — bare, as a
// list element, an optional payload, a map key: the bytes, widths,
// max_encoded_size and Schema IR node of psio::utf8_string<80> (a named
// alias of the bounded UTF-8 string), and the same checks in every codec
// (the writers refuse a value outside the bound or not UTF-8, the readers
// reject its bytes). It is the one rule of every language (Rust:
// `#[psio(transparent)] struct Memo(#[psio(max = 80)] String)`).
//
// Any other member annotation is a compile error, because the wrapper
// would differ from its member in a way a type cannot say: exact<N> /
// min<N> (a member attribute in the IR, and a transparent type has no
// member there), sorted / unique, the integer text forms, as<Tag>, and
// max<N> on a type with no bounded form (psio::utf8_string, a nested
// value: the bound is already in the type). State such a constraint as the
// type's invariant.
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
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

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

      // What the member of a transparent type may be annotated with: only
      // what turns its type into a bounded type (see the header comment).
      template <typename M>
      inline constexpr bool transparent_bound_form_v = false;
      template <typename C, typename Tr, typename A>
      inline constexpr bool transparent_bound_form_v<std::basic_string<C, Tr, A>> = true;
      template <typename E, typename A>
      inline constexpr bool transparent_bound_form_v<std::vector<E, A>> = true;
      template <typename M>
         requires requires { typename M::key_type; }
      inline constexpr bool transparent_bound_form_v<M> = true;

      template <typename S>
      inline constexpr bool transparent_position_spec_v = false;
      template <typename C>
      inline constexpr bool transparent_position_spec_v<element_spec<C>> = true;
      template <typename C>
      inline constexpr bool transparent_position_spec_v<keys_spec<C>> = true;
      template <typename C>
      inline constexpr bool transparent_position_spec_v<values_spec<C>> = true;
      template <std::size_t I, typename C>
      inline constexpr bool transparent_position_spec_v<at_spec<I, C>> = true;

      template <typename M, typename... Ss>
      constexpr bool transparent_member_specs_ok(const std::tuple<Ss...>& specs) noexcept
      {
         // Only bounds, utf8 and position specs (whose own content
         // effective_annotations_for checks against the member's shape).
         if (!((std::is_same_v<Ss, length_bound> || std::is_same_v<Ss, utf8_spec> ||
                transparent_position_spec_v<Ss>) &&
               ... && true))
            return false;
         if constexpr ((std::is_same_v<Ss, length_bound> || ...))
         {
            const length_bound lb = std::get<length_bound>(specs);
            if (lb.exact || lb.min || !lb.max || lb.conflict)
               return false;
            if (!transparent_bound_form_v<M>)
               return false;
         }
         if constexpr ((std::is_same_v<Ss, utf8_spec> || ...))
            if (!std::is_same_v<M, std::string>)
               return false;
         return true;
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
               transparent_member_specs_ok<std::remove_cv_t<typename R::template member_type<0>>>(
                  detail::member_annotations_in<T, R::template member_pointer<0>>()),
               "psio: the member of a transparent() type takes only the annotations that "
               "make it a bounded type: utf8 on a std::string, max<N> on a std::string, "
               "std::vector, map or set, and element / keys / values / at<I>(...) below it. "
               "exact / min, sorted / unique, integer text forms and as<Tag> are not a type: "
               "declare an invariant");
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
