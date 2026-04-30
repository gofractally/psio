#pragma once
//
// psio/reflect.hpp — Layer 1 reflection trait.
//
// Exposes per-type metadata through psio::reflect<T>:
//
//   psio::reflect<T>::name                           struct name (string_view)
//   psio::reflect<T>::member_count                   number of fields
//   psio::reflect<T>::member_pointer<I>              &T::field_I
//   psio::reflect<T>::member_name<I>                 "field_I"
//   psio::reflect<T>::field_number<I>                1-based ordinal (source order)
//   psio::reflect<T>::member_type<I>                 type of field I
//   psio::reflect<T>::index_of(name)                 runtime name → index
//   psio::reflect<T>::index_of_field_number(n)       field-number → index
//   psio::reflect<T>::visit_field_by_name(obj, name, fn)
//   psio::reflect<T>::visit_field_by_number(obj, n, fn)
//   psio::reflect<T>::for_each_field(obj, fn)        iterate all
//   psio::reflect<T>::is_reflected                   SFINAE-style check
//
// Population: `PSIO_REFLECT(Type, field1, field2, ...)` macro, callable
// at namespace scope right after the struct definition (no namespace
// gymnastics needed — macro uses ADL to plug the metadata into the
// user's namespace).
//
// Dispatch: `psio::reflect<T>` is a class template whose base is the
// type returned by an ADL-found helper `psio_reflect_helper(T*)`.
// Unreflected types fall through to an empty default with
// is_reflected = false.
//
// Reflection covers public non-static data members only (design § 5.2.5.4).

#include <boost/preprocessor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psio {

   namespace detail {
      struct reflect_default_impl {
         static constexpr bool is_reflected = false;
      };

      // Reasonable linear scan — sufficient for structs up to BeaconState
      // scale (~20 fields) at no measurable runtime cost. A true
      // perfect-hash backend lands in phase 13 (design § 5.2.5.1); the
      // public surface does not change.
      template <std::size_t N>
      constexpr std::optional<std::size_t>
      linear_index_of(const std::array<std::string_view, N>& names,
                      std::string_view target) noexcept
      {
         for (std::size_t i = 0; i < N; ++i)
            if (names[i] == target) return i;
         return std::nullopt;
      }
   }

   // Fallback ADL hook: returns the default empty impl for unreflected
   // types. Users' PSIO_REFLECT macro emits a more-specific overload
   // in their namespace; ADL picks that one over this fallback.
   template <typename T>
   constexpr detail::reflect_default_impl psio_reflect_helper(T*) noexcept
   {
      return {};
   }

   // Public facade. Inherits from whatever the ADL dispatch returns —
   // a detail::reflect_default_impl for unreflected types, or the
   // user's generated struct for reflected types.
   template <typename T>
   struct reflect : decltype(psio_reflect_helper(static_cast<T*>(nullptr)))
   {
   };

   // Concept gate.
   template <typename T>
   concept Reflected = reflect<T>::is_reflected;

}  // namespace psio

// ── Macro implementation ─────────────────────────────────────────────────
//
// PSIO_REFLECT(Type, f1, f2, ...) emits an ADL-found helper function
// in the enclosing namespace:
//
//     inline auto psio_reflect_helper(Type*) noexcept { return … impl … ; }
//
// The returned unnamed struct carries all the metadata psio::reflect<T>
// exposes. Because the helper lives in the same namespace as the struct,
// users can call PSIO_REFLECT right after the struct definition without
// leaving the namespace.
//
// Each per-field helper takes (r, data, I, FIELD).

#define PSIO_REFLECT_MEMBER_POINTER_CASE_(r, TYPE, I, FIELD) \
   if constexpr (Idx == (I)) return &TYPE::FIELD;

#define PSIO_REFLECT_MEMBER_NAME_CASE_(r, _, I, FIELD) \
   if constexpr (Idx == (I)) return ::std::string_view(BOOST_PP_STRINGIZE(FIELD));

#define PSIO_REFLECT_MEMBER_TYPE_CASE_(r, TYPE, I, FIELD)                                    \
   if constexpr (Idx == (I))                                                                  \
      return ::std::type_identity<                                                            \
         ::std::remove_reference_t<decltype(::std::declval<TYPE&>().FIELD)>>{};

#define PSIO_REFLECT_NAME_ARRAY_ELEM_(r, _, I, FIELD) \
   BOOST_PP_COMMA_IF(I) ::std::string_view(BOOST_PP_STRINGIZE(FIELD))

#define PSIO_REFLECT_VISIT_NAME_CASE_(r, _, I, FIELD)                \
   if (lookup_name == ::std::string_view(BOOST_PP_STRINGIZE(FIELD)))  \
   {                                                                  \
      ::std::invoke(::std::forward<F>(fn), obj.FIELD);                \
      return true;                                                    \
   }

#define PSIO_REFLECT_VISIT_NUMBER_CASE_(r, _, I, FIELD)  \
   if (lookup_num == static_cast<::std::uint32_t>((I) + 1)) \
   {                                                      \
      ::std::invoke(::std::forward<F>(fn), obj.FIELD);    \
      return true;                                        \
   }

#define PSIO_REFLECT_FOR_EACH_CASE_(r, _, I, FIELD)              \
   ::std::invoke(::std::forward<F>(fn),                           \
                 ::std::integral_constant<::std::size_t, (I)>{},  \
                 ::std::string_view(BOOST_PP_STRINGIZE(FIELD)),   \
                 obj.FIELD);

// NOTE: this macro takes an *unqualified* type identifier. Invoke
// PSIO_REFLECT inside the namespace containing the type:
//
//     namespace eth {
//        struct Validator { ... };
//        PSIO_REFLECT(Validator, pubkey, withdrawal_credentials, ...)
//     }
//
// The macro concatenates the type identifier into a helper struct
// name, so TYPE must be a simple identifier (no `::`, no templates).
// Templated types and qualified names will be supported via a larger
// macro expansion in a later phase; today's phase-1 constraint covers
// all of v1 psio's existing callers.

// ── Keyword dispatch ──────────────────────────────────────────────────────
//
// PSIO_REFLECT accepts five kinds of arguments:
//
//   1. A bare identifier              `pubkey`
//      → registered as a field, name = identifier
//
//   2. attr(name, spec_expr)          `attr(items, max<255> | field<3>)`
//      → registered as a field AND emits the field's annotation
//      specialisation. `spec_expr` is evaluated inside a constexpr lambda
//      with `using namespace ::psio;` in scope so spec helpers
//      (`max<N>`, `field<N>`, `bytes<N>`, `utf8<N>`, `sorted`, `unique`,
//      ...) are available unqualified.
//
//   3. definitionWillNotChange()      type-level keyword
//      → emits the type-level `psio::definition_will_not_change`
//      annotation, contributes nothing to the field list.  Same name as
//      v1 reflect's flag for source-compatibility.
//
//   4. maxFields(N)                   type-level keyword
//      → emits the type-level `psio::max_fields_spec{N}` annotation.
//      Caps the number of declared fields a record may have; queryable
//      via `psio::max_fields_v<T>` and `psio::effective_max_fields_v<T>`.
//
//   5. maxDynamicData(N)              type-level keyword
//      → emits the type-level `psio::max_dynamic_data_spec{N}`
//      annotation.  Caps the total encoded payload size in bytes.
//      Queryable via `psio::max_dynamic_data_v<T>` and
//      `psio::effective_max_dynamic_v<T>`.  Format encode/validate paths
//      enforce the cap (throw on encode excess, reject on validate).
//
// All type-level keywords (3-5) are aggregated into one
// `annotate<type<T>{}>` specialization via `std::tuple_cat`, so they
// compose freely.  Per-keyword paren form (`maxFields(N)`, NOT
// `maxFields<N>()`) is required because the cat-detect dispatch can't
// handle `<` in macro names.
//
// The dispatch uses the v1 PSIO1_MATCH cat-detect pattern: each keyword
// is recognised by pasting its spelling onto a known prefix and seeing
// whether the resulting macro is defined.  Bare identifiers fall through
// to the "field" handler.  Adding a new keyword = one macro definition.
//
// `attr`, `definitionWillNotChange`, `maxFields`, `maxDynamicData` are
// global preprocessor names.  If they collide with a user identifier in
// the same TU, `#undef` after the REFLECT block.

#define PSIO_PP_FIRST(a, ...) a
#define PSIO_PP_APPLY_FIRST(a) PSIO_PP_FIRST(a)
#define PSIO_PP_MATCH(prefix, x) PSIO_PP_MATCH_CHECK(BOOST_PP_CAT(prefix, x))
#define PSIO_PP_MATCH_CHECK(...) PSIO_PP_MATCH_CHECK_N(__VA_ARGS__, 0, )
#define PSIO_PP_MATCH_CHECK_N(x, n, r, ...) \
   BOOST_PP_BITAND(n, BOOST_PP_COMPL(BOOST_PP_CHECK_EMPTY(r)))

// Match table.  Each defined entry expands to `(KIND, payload...), 1`
// where KIND is one of `attrfield` / `typeattr_dwnc`.
//
//   - attr(F, spec_expr)            → (attrfield, F, spec_expr), 1
//   - definitionWillNotChange()     → (typeattr_dwnc), 1
//
// Bare identifiers leave the cat unexpanded → PSIO_PP_MATCH returns 0.
#define PSIO_REFLECT_KW_attr(F, ...) (attrfield, F, __VA_ARGS__), 1
#define PSIO_REFLECT_KW_definitionWillNotChange(...) (typeattr_dwnc), 1
// Type-level caps. Note: keyword form takes the bound as a function
// argument — `maxFields(N)` not `maxFields<N>()` — because the macro
// dispatch cat-detects on `PSIO_REFLECT_KW_<identifier>` and `<` isn't
// valid in macro names.
#define PSIO_REFLECT_KW_maxFields(N) (typeattr_max_fields, N), 1
#define PSIO_REFLECT_KW_maxDynamicData(N) (typeattr_max_dyn_data, N), 1

// Classify each item into `(KIND, payload...)`.  Bare ident → (field, ident).
#define PSIO_REFLECT_CLASSIFY(s, _, item)                                  \
   BOOST_PP_IIF(PSIO_PP_MATCH(PSIO_REFLECT_KW_, item),                    \
                PSIO_REFLECT_CLASSIFY_KW,                                  \
                PSIO_REFLECT_CLASSIFY_PLAIN)(item)
#define PSIO_REFLECT_CLASSIFY_KW(item) \
   PSIO_PP_APPLY_FIRST(BOOST_PP_CAT(PSIO_REFLECT_KW_, item))
#define PSIO_REFLECT_CLASSIFY_PLAIN(item) (field, item)

// Field-list filter: keep `field` and `attrfield`; drop `typeattr_*`.
#define PSIO_REFLECT_KEEP_FIELD(s, _, kt) \
   BOOST_PP_BITOR(BOOST_PP_EQUAL(0, BOOST_PP_CAT(PSIO_REFLECT_KIND_, BOOST_PP_TUPLE_ELEM(0, kt))), \
                  BOOST_PP_EQUAL(1, BOOST_PP_CAT(PSIO_REFLECT_KIND_, BOOST_PP_TUPLE_ELEM(0, kt))))
// Map kind tags to small integers so we can compare without strcmp.
#define PSIO_REFLECT_KIND_field                  0
#define PSIO_REFLECT_KIND_attrfield              1
#define PSIO_REFLECT_KIND_typeattr_dwnc          2
#define PSIO_REFLECT_KIND_typeattr_max_fields    3
#define PSIO_REFLECT_KIND_typeattr_max_dyn_data  4

// Field-name extractor.  For `(field, F)` and `(attrfield, F, ...)`
// the name is element 1 of the tuple.
#define PSIO_REFLECT_NAME_OF(s, _, kt) BOOST_PP_TUPLE_ELEM(1, kt)

// Annotation emitter.  Per-tuple dispatch via cat to a kind-specific
// emitter that takes (TYPE, kt).  For `field` kind, emits nothing.
#define PSIO_REFLECT_EMIT_ANN(s, TYPE, kt) \
   BOOST_PP_CAT(PSIO_REFLECT_EMIT_ANN_, BOOST_PP_TUPLE_ELEM(0, kt))(TYPE, kt)

#define PSIO_REFLECT_EMIT_ANN_field(TYPE, kt) /* no-op */

#define PSIO_REFLECT_EMIT_ANN_attrfield(TYPE, kt)                              \
   template <>                                                                  \
   inline constexpr auto ::psio::annotate<&TYPE::BOOST_PP_TUPLE_ELEM(1, kt)> = \
      [] {                                                                      \
         using namespace ::psio;                                                 \
         return ::psio::to_spec_tuple(BOOST_PP_TUPLE_ELEM(2, kt));              \
      }();

// Per-item emit for type-level kinds is a no-op: all type-level
// annotations are aggregated into ONE specialization via the
// PSIO_REFLECT_EMIT_TYPE_AGG pass below, so combinations like
// `definitionWillNotChange(), maxFields(5), maxDynamicData(4096)`
// compose into a single std::tuple instead of fighting over the
// `annotate<type<T>{}>` slot.
#define PSIO_REFLECT_EMIT_ANN_typeattr_dwnc(TYPE, kt) /* aggregated */
#define PSIO_REFLECT_EMIT_ANN_typeattr_max_fields(TYPE, kt) /* aggregated */
#define PSIO_REFLECT_EMIT_ANN_typeattr_max_dyn_data(TYPE, kt) /* aggregated */

// Per-kind transform: emit `, std::tuple{spec_value}` for each
// type-level entry, nothing for fields / attrfields.  Concatenated
// inside std::tuple_cat(...) to form the aggregated annotation tuple.
// Leading comma is harmless because the cat call always seeds with an
// empty tuple sentinel — see PSIO_REFLECT_EMIT_TYPE_AGG.
#define PSIO_REFLECT_TYPEATTR_TUPLE(s, _, kt)                                  \
   BOOST_PP_CAT(PSIO_REFLECT_TYPEATTR_TUPLE_, BOOST_PP_TUPLE_ELEM(0, kt))(kt)
#define PSIO_REFLECT_TYPEATTR_TUPLE_field(kt)                                  /**/
#define PSIO_REFLECT_TYPEATTR_TUPLE_attrfield(kt)                              /**/
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_dwnc(kt)                          \
   , ::std::tuple{::psio::definition_will_not_change{}}
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_max_fields(kt)                    \
   , ::std::tuple{::psio::max_fields_spec{BOOST_PP_TUPLE_ELEM(1, kt)}}
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_max_dyn_data(kt)                  \
   , ::std::tuple{::psio::max_dynamic_data_spec{BOOST_PP_TUPLE_ELEM(1, kt)}}

// Per-kind presence marker: emit a token if the entry is type-level,
// nothing otherwise.  Concatenated to detect whether KIND_SEQ contains
// any type-level keywords.  When the resulting expansion is empty,
// `BOOST_PP_CHECK_EMPTY` returns 1 and the aggregator suppresses its
// `annotate<type<T>{}>` specialization — leaving room for an external
// `PSIO_TYPE_ATTRS(T, …)` call (or for the primary-template default
// to be used).
#define PSIO_REFLECT_TYPEATTR_PRESENT(s, _, kt)                                \
   BOOST_PP_CAT(PSIO_REFLECT_TYPEATTR_PRESENT_, BOOST_PP_TUPLE_ELEM(0, kt))
#define PSIO_REFLECT_TYPEATTR_PRESENT_field                                    /**/
#define PSIO_REFLECT_TYPEATTR_PRESENT_attrfield                                /**/
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_dwnc          1
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_max_fields    1
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_max_dyn_data  1
#define PSIO_REFLECT_TYPEATTR_HAS_(KIND_SEQ)                                   \
   BOOST_PP_SEQ_FOR_EACH(PSIO_REFLECT_TYPEATTR_PRESENT, _, KIND_SEQ)

// Aggregator: emit one `annotate<type<TYPE>{}>` specialization whose
// value is `tuple_cat(tuple<>{}, <per-item tuples>)`.  Skips entirely
// when no type-level keywords are present, so types reflected without
// any of {definitionWillNotChange(), maxFields(N), maxDynamicData(N)}
// keep using the primary `annotate<…> = std::tuple<>{}` template, and
// callers may attach type-level annotations later via PSIO_TYPE_ATTRS.
#define PSIO_REFLECT_EMIT_TYPE_AGG(TYPE, KIND_SEQ)                             \
   BOOST_PP_IIF(                                                               \
      BOOST_PP_COMPL(                                                          \
         BOOST_PP_CHECK_EMPTY(PSIO_REFLECT_TYPEATTR_HAS_(KIND_SEQ))),          \
      PSIO_REFLECT_EMIT_TYPE_AGG_DO,                                           \
      PSIO_REFLECT_EMIT_TYPE_AGG_NOOP)(TYPE, KIND_SEQ)

#define PSIO_REFLECT_EMIT_TYPE_AGG_NOOP(TYPE, KIND_SEQ)                        /**/
#define PSIO_REFLECT_EMIT_TYPE_AGG_DO(TYPE, KIND_SEQ)                          \
   template <>                                                                 \
   inline constexpr auto ::psio::annotate<::psio::type<TYPE>{}> =              \
      ::std::tuple_cat(                                                        \
         ::std::tuple<>{}                                                      \
         BOOST_PP_SEQ_FOR_EACH(PSIO_REFLECT_TYPEATTR_TUPLE, _, KIND_SEQ));

// PSIO_REFLECT(TYPE)             — zero-field marker (resource opt-in)
// PSIO_REFLECT(TYPE, f1, f2, …)  — record / struct
//
// Dispatch on __VA_OPT__: BOOST_PP_VARIADIC_TO_SEQ on truly-empty input
// produces a one-element seq with empty content, which then trips the
// downstream macros. Splitting on emptiness avoids the issue.
#define PSIO_REFLECT(TYPE, ...) \
   BOOST_PP_CAT(PSIO_REFLECT_DISPATCH_, \
                BOOST_PP_CAT(0, __VA_OPT__(_HAS)))(TYPE, __VA_ARGS__)

#define PSIO_REFLECT_DISPATCH_0(TYPE, ...) PSIO_REFLECT_NO_FIELDS_(TYPE)
#define PSIO_REFLECT_DISPATCH_0_HAS(TYPE, ...)                                         \
   PSIO_REFLECT_DISPATCH_(                                                              \
      TYPE,                                                                             \
      BOOST_PP_SEQ_TRANSFORM(PSIO_REFLECT_CLASSIFY, _,                                  \
                             BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))

// Zero-field reflection — used for resource markers like
//
//    struct pollable : psio::wit_resource {};
//    PSIO_REFLECT(pollable)
//
// The reflected metadata reports member_count = 0 and short-circuits
// the visit / for_each entry points.  No member_pointer / member_name
// / member_type templates are instantiated (Idx has no valid value),
// so callers gating on member_count > 0 stay correct.
#define PSIO_REFLECT_NO_FIELDS_(TYPE)                                                  \
   struct BOOST_PP_CAT(psio_reflect_impl_, TYPE)                                      \
   {                                                                                   \
      using type = TYPE;                                                               \
      static constexpr bool               is_reflected = true;                         \
      static constexpr ::std::string_view name         = BOOST_PP_STRINGIZE(TYPE);     \
      static constexpr ::std::size_t      member_count = 0;                            \
      static constexpr ::std::array<::std::string_view, 0> _names_array_{};            \
                                                                                       \
      static constexpr ::std::optional<::std::size_t>                                  \
      index_of(::std::string_view) noexcept                                            \
      {                                                                                \
         return ::std::nullopt;                                                        \
      }                                                                                \
                                                                                       \
      static constexpr ::std::optional<::std::size_t>                                  \
      index_of_field_number(::std::uint32_t) noexcept                                  \
      {                                                                                \
         return ::std::nullopt;                                                        \
      }                                                                                \
                                                                                       \
      template <typename Obj, typename F>                                              \
      static constexpr bool visit_field_by_name(Obj&, ::std::string_view, F&&)         \
      {                                                                                \
         return false;                                                                 \
      }                                                                                \
                                                                                       \
      template <typename Obj, typename F>                                              \
      static constexpr bool visit_field_by_number(Obj&, ::std::uint32_t, F&&)          \
      {                                                                                \
         return false;                                                                 \
      }                                                                                \
                                                                                       \
      template <typename Obj, typename F>                                              \
      static constexpr void for_each_field(Obj&, F&&)                                  \
      {                                                                                \
      }                                                                                \
   };                                                                                  \
   inline auto psio_reflect_helper(TYPE*) noexcept                                    \
   {                                                                                   \
      return BOOST_PP_CAT(psio_reflect_impl_, TYPE){};                                \
   }

#define PSIO_REFLECT_DISPATCH_(TYPE, KIND_SEQ)                                           \
   PSIO_REFLECT_IMPL_(                                                                   \
      TYPE,                                                                               \
      BOOST_PP_SEQ_TRANSFORM(                                                             \
         PSIO_REFLECT_NAME_OF, _,                                                        \
         BOOST_PP_SEQ_FILTER(PSIO_REFLECT_KEEP_FIELD, _, KIND_SEQ)))                     \
   BOOST_PP_SEQ_FOR_EACH(PSIO_REFLECT_EMIT_ANN, TYPE, KIND_SEQ)                          \
   PSIO_REFLECT_EMIT_TYPE_AGG(TYPE, KIND_SEQ)

#define PSIO_REFLECT_IMPL_(TYPE, SEQ)                                                   \
   struct BOOST_PP_CAT(psio_reflect_impl_, TYPE)                                        \
   {                                                                                     \
      using type = TYPE;                                                                 \
      static constexpr bool               is_reflected = true;                           \
      static constexpr ::std::string_view name         = BOOST_PP_STRINGIZE(TYPE);       \
      static constexpr ::std::size_t      member_count = BOOST_PP_SEQ_SIZE(SEQ);         \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr auto _member_pointer_impl_() noexcept                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_MEMBER_POINTER_CASE_, TYPE, SEQ)          \
      }                                                                                  \
      template <::std::size_t Idx>                                                       \
      static constexpr auto member_pointer = _member_pointer_impl_<Idx>();               \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::string_view _member_name_impl_() noexcept                  \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_MEMBER_NAME_CASE_, _, SEQ)                \
      }                                                                                  \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::string_view member_name = _member_name_impl_<Idx>();       \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr auto _member_type_impl_() noexcept                                \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_MEMBER_TYPE_CASE_, TYPE, SEQ)             \
      }                                                                                  \
      template <::std::size_t Idx>                                                       \
      using member_type = typename decltype(_member_type_impl_<Idx>())::type;            \
                                                                                         \
      /* Source-order 1-based field numbers. Annotations (phase 2) */                    \
      /* override per-field via psio::annotate<&T::m>.             */                   \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::uint32_t field_number                                      \
         = static_cast<::std::uint32_t>(Idx + 1);                                        \
                                                                                         \
      static constexpr ::std::array<::std::string_view, member_count> _names_array_      \
         = {BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_NAME_ARRAY_ELEM_, _, SEQ)};             \
                                                                                         \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of(::std::string_view name_) noexcept                                        \
      {                                                                                  \
         return ::psio::detail::linear_index_of(_names_array_, name_);                  \
      }                                                                                  \
                                                                                         \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of_field_number(::std::uint32_t n) noexcept                                  \
      {                                                                                  \
         if (n < 1 || n > member_count) return ::std::nullopt;                           \
         return static_cast<::std::size_t>(n - 1);                                       \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_name(                                         \
         Obj& obj, ::std::string_view lookup_name, F&& fn)                               \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_VISIT_NAME_CASE_, _, SEQ)                 \
         return false;                                                                   \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_number(                                       \
         Obj& obj, ::std::uint32_t lookup_num, F&& fn)                                   \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_VISIT_NUMBER_CASE_, _, SEQ)               \
         return false;                                                                   \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr void for_each_field(Obj& obj, F&& fn)                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_FOR_EACH_CASE_, _, SEQ)                   \
      }                                                                                  \
   };                                                                                    \
   inline auto psio_reflect_helper(TYPE*) noexcept                                      \
   {                                                                                     \
      return BOOST_PP_CAT(psio_reflect_impl_, TYPE){};                                  \
   }
