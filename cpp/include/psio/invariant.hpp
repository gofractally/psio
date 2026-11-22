#pragma once
//
// psio/invariant.hpp — per-type semantic invariants.
//
// A type declares ONE check over a decoded value:
//
//     struct AccountNumber {
//        std::uint64_t value = 0;
//        static psio::codec_status check(const AccountNumber& a) noexcept {
//           return canonical(a.value)
//                     ? psio::codec_ok()
//                     : psio::codec_fail("account name is not in canonical form", 0,
//                                        "AccountNumber");
//        }
//        PSIO_REFLECT(AccountNumber, definitionWillNotChange(), invariant(check), value)
//     };
//
// `invariant(fn)` is a PSIO_REFLECT keyword (reflect.hpp): fn is any
// expression callable as `fn(const T&) noexcept` returning
// `psio::codec_status`. A type that is not a reflected record — an enum, a
// std::variant, a std::tuple, any class type — registers its check with
//
//     PSIO_INVARIANT(Type, fn)               // at global scope
//     PSIO_INVARIANT(Type, fn, "SchemaId")   // and names its schema node
//
// (Type is one macro argument: name a template-id with commas through an
// alias. The registration is for that exact C++ type wherever it is used:
// an alias is not a type; a distinct identity is a transparent wrapper,
// psio/transparent.hpp.) Arithmetic types take no invariant. A type has at
// most one: declared twice is a compile error.
//
//   psio::has_invariant_v<T>            T itself has an invariant
//   psio::check_invariant(value)        runs it; ok when T has none
//   psio::contains_invariant_v<T>       T, or any type reachable through its
//                                       members / elements / alternatives, has
//                                       one
//   psio::any_has_invariant_v<Ts...>    one of Ts has an invariant (a path of
//                                       enclosing types)
//   psio::invariant_children<T>         extension point of the transitive
//                                       query (see below)
//
// The status the check returns is the type's own and is passed through
// unchanged by every codec: by convention
// `codec_fail(message, 0, "<type name>")`.
// A message that names the offending value is made with
// `codec_fail_owned(text, 0, "<type name>")` (error.hpp): the text is copied
// once, on the failing path only, and travels with the status (and with a
// codec_exception made from it). A fixed message stays a literal.
//
// The contract of a check. It is called on every value of the type that is
// read or written through a checking entry point, including on hostile
// input, so it must (1) be noexcept and return its verdict as the status —
// the macros reject a check that is not `noexcept` or does not return
// `psio::codec_status`; (2) not throw (a throw out of a noexcept function
// is std::terminate) and not abort: a check built on a library that throws
// or aborts on malformed data must catch that or test the data first,
// because the status-returning entry points promise never to do either, in
// builds with and without exceptions; (3) depend only on the value. The
// same holds for a text adapter's `validate` / `decode` on text (adapter.hpp).
//
// Where it runs (docs/pssz-spec.md "Type invariants"): every reader that
// checks its input and every writer of the core formats — pssz validate /
// validate_or_throw / validate_strict / validate_pssz_compatible /
// decode_pssz_checked / encode / encode_pssz_checked; key encode, validate
// and decode; json / ijson encode and every reader. After the value's
// structure is accepted the value is materialised and checked, innermost
// first, so a type's check may rely on those of its members. It does NOT
// run in the trusted pssz `psio::decode` / `make_boxed` and in views (they
// assume validated bytes), nor in the other formats. A type without an
// invariant costs nothing.

#include <psio/error.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace psio::schema_types
{
   template <typename T>
   struct Box;  // schema_ir.hpp: T on the wire
}  // namespace psio::schema_types

namespace psio
{
   // The registration of PSIO_INVARIANT. The primary template says "none".
   template <typename T>
   struct invariant_of
   {
      static constexpr bool has_value = false;
   };

   namespace detail
   {
      template <typename T>
      constexpr bool reflected_invariant() noexcept
      {
         if constexpr (std::is_class_v<T> && Reflected<T>)
         {
            if constexpr (requires { reflect<T>::_psio_invariant_count_; })
               return reflect<T>::_psio_invariant_count_ == 1;
            else
               return false;
         }
         else
            return false;
      }

      template <typename T>
      constexpr bool invariant_declared() noexcept
      {
         static_assert(!(reflected_invariant<T>() && invariant_of<T>::has_value),
                       "psio: the invariant of this type is declared twice — invariant(fn) "
                       "in PSIO_REFLECT and PSIO_INVARIANT");
         return reflected_invariant<T>() || invariant_of<T>::has_value;
      }
   }  // namespace detail

   template <typename T>
   inline constexpr bool has_invariant_v =
      detail::invariant_declared<std::remove_cvref_t<T>>();

   // One of the types has an invariant: `any_has_invariant_v<Root, Member,
   // Leaf>` is "a leaf reached through these types sits inside a value an
   // invariant constrains".
   template <typename... Ts>
   inline constexpr bool any_has_invariant_v = (has_invariant_v<Ts> || ...);

   // The check of T on `value`; ok for a type without one.
   template <typename T>
   [[nodiscard]] codec_status check_invariant(const T& value) noexcept
   {
      if constexpr (detail::reflected_invariant<T>())
      {
         using R = reflect<T>;
         return R::_psio_invariant_(
            std::integral_constant<std::size_t, R::_psio_invariant_index_>{}, value);
      }
      else if constexpr (invariant_of<T>::has_value)
         return invariant_of<T>::check(value);
      else
      {
         (void)value;
         return codec_ok();
      }
   }

   // The schema id PSIO_INVARIANT gave T, or empty.
   template <typename T>
   constexpr std::string_view invariant_schema_id() noexcept
   {
      using U = std::remove_cvref_t<T>;
      if constexpr (requires { std::string_view{invariant_of<U>::schema_id}; })
         return std::string_view{invariant_of<U>::schema_id};
      else
         return {};
   }

   // ── Transitive query ───────────────────────────────────────────────────
   //
   // `invariant_children<T>::type` is a std::tuple of the types a value of T
   // holds values of. The primary template covers reflected records (their
   // members), tuple-likes (std::tuple, std::pair, std::array: their
   // elements), std::variant (its alternatives) and anything with a
   // `value_type` (std::vector, std::optional, the maps and sets, bounded
   // lists). A value kind none of these describes specializes it:
   //
   //     template <typename T>
   //     struct psio::invariant_children<my_holder<T>> { using type = std::tuple<T>; };
   template <typename T, typename = void>
   struct invariant_children
   {
      using type = std::tuple<>;
   };

   namespace detail
   {
      template <typename T>
      inline constexpr bool is_tuple_like_v = false;
      template <typename... Ts>
      inline constexpr bool is_tuple_like_v<std::tuple<Ts...>> = true;
      template <typename A, typename B>
      inline constexpr bool is_tuple_like_v<std::pair<A, B>> = true;
      template <typename T, std::size_t N>
      inline constexpr bool is_tuple_like_v<std::array<T, N>> = true;

      template <typename T>
      inline constexpr bool is_variant_like_v = false;
      template <typename... Ts>
      inline constexpr bool is_variant_like_v<std::variant<Ts...>> = true;

      template <typename T, typename Seq>
      struct reflected_member_types;
      template <typename T, std::size_t... Is>
      struct reflected_member_types<T, std::index_sequence<Is...>>
      {
         using type = std::tuple<
            std::remove_cv_t<typename reflect<T>::template member_type<Is>>...>;
      };
      template <typename T, typename Seq>
      struct tuple_element_types;
      template <typename T, std::size_t... Is>
      struct tuple_element_types<T, std::index_sequence<Is...>>
      {
         using type = std::tuple<std::remove_cv_t<std::tuple_element_t<Is, T>>...>;
      };
      template <typename T>
      struct variant_alternative_types;
      template <typename... Ts>
      struct variant_alternative_types<std::variant<Ts...>>
      {
         using type = std::tuple<Ts...>;
      };

      template <typename T>
      concept reflected_class = std::is_class_v<T> && Reflected<T>;
   }  // namespace detail

   template <detail::reflected_class T>
   struct invariant_children<T, void>
      : detail::reflected_member_types<T, std::make_index_sequence<reflect<T>::member_count>>
   {
   };
   template <typename T>
      requires(!detail::reflected_class<T> && detail::is_tuple_like_v<T>)
   struct invariant_children<T, void>
      : detail::tuple_element_types<T, std::make_index_sequence<std::tuple_size_v<T>>>
   {
   };
   template <typename T>
      requires(!detail::reflected_class<T> && detail::is_variant_like_v<T>)
   struct invariant_children<T, void> : detail::variant_alternative_types<T>
   {
   };
   template <typename T>
      requires(!detail::reflected_class<T> && !detail::is_tuple_like_v<T> &&
               !detail::is_variant_like_v<T> && requires { typename T::value_type; })
   struct invariant_children<T, void>
   {
      using type = std::tuple<std::remove_cv_t<typename T::value_type>>;
   };

   template <typename T>
   struct invariant_children<schema_types::Box<T>, void>
   {
      using type = std::tuple<T>;
   };

   namespace detail
   {
      template <typename T, typename... Seen>
      constexpr bool contains_invariant() noexcept;

      template <typename Tuple, typename... Seen>
      struct any_child_contains;
      template <typename... Cs, typename... Seen>
      struct any_child_contains<std::tuple<Cs...>, Seen...>
      {
         static constexpr bool value = (contains_invariant<Cs, Seen...>() || ...);
      };

      // `Seen` closes a cycle of the type graph (a recursive type).
      template <typename T, typename... Seen>
      constexpr bool contains_invariant() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr ((std::is_same_v<U, Seen> || ...))
            return false;
         else if constexpr (std::is_arithmetic_v<U>)
            return false;
         else if constexpr (has_invariant_v<U>)
            return true;
         else
            return any_child_contains<typename invariant_children<U>::type, Seen..., U>::value;
      }
   }  // namespace detail

   template <typename T>
   inline constexpr bool contains_invariant_v = detail::contains_invariant<T>();

}  // namespace psio

// ── PSIO_INVARIANT ───────────────────────────────────────────────────────
//
// At global scope. `FN` is looked up where the macro is written (qualify a
// name that lives in a namespace); parenthesize an expression that has a
// top-level comma. The optional third argument is the type's schema id.
#define PSIO_INVARIANT(TYPE, FN, ...)                                                       \
   namespace psio                                                                           \
   {                                                                                        \
      template <>                                                                           \
      struct invariant_of<TYPE>                                                             \
      {                                                                                     \
         static constexpr bool has_value = true;                                            \
         __VA_OPT__(static constexpr ::std::string_view schema_id = __VA_ARGS__;)           \
         static_assert(::std::is_class_v<TYPE> || ::std::is_enum_v<TYPE>,                   \
                       "psio: PSIO_INVARIANT takes a class or an enum type");               \
         static_assert(                                                                     \
            requires(const TYPE& v) {                                                       \
               { (FN)(v) } noexcept -> ::std::same_as<::psio::codec_status>;                \
            },                                                                              \
            "psio: PSIO_INVARIANT(Type, fn) needs fn callable as fn(const Type&), "         \
            "declared noexcept and returning psio::codec_status");                          \
         [[nodiscard]] static ::psio::codec_status check(const TYPE& v) noexcept            \
         {                                                                                  \
            return (FN)(v);                                                                 \
         }                                                                                  \
      };                                                                                    \
   }
