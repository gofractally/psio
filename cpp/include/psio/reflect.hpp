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
//   psio::reflect<T>::index_of(name)                 name → index (optional)
//   psio::reflect<T>::find_index(name)               name → index, member_count if absent
//   psio::reflect<T>::index_of_field_number(n)       field-number → index
//   psio::reflect<T>::index_of_hash<Hash>(key)       Hash{}(name) == key → index
//   psio::reflect<T>::visit_field_by_name(obj, name, fn)
//   psio::reflect<T>::visit_field_by_number(obj, n, fn)
//   psio::reflect<T>::for_each_field(obj, fn)        iterate all
//   psio::reflect<T>::is_reflected                   SFINAE-style check
//
// Methods (listed with `method(name, param...)`):
//
//   psio::reflect<T>::method_count
//   psio::reflect<T>::method_pointer<I>              &T::method_I (a value)
//   psio::reflect<T>::method_type<I>                 its type
//   psio::reflect<T>::method_name<I> / method_names  "method_I" / std::array of all
//   psio::reflect<T>::method_param_names<I>          std::array<string_view, K> as written
//   psio::reflect<T>::method_result<I>               return type
//   psio::reflect<T>::method_args<I>                 std::tuple<Args...> as declared
//   psio::reflect<T>::method_params<I>               std::tuple<remove_cvref_t<Args>...>
//   psio::reflect<T>::method_is_static<I> / method_is_const<I>
//   psio::reflect<T>::method_traits<I>               psio::method_traits<method_type<I>>
//   psio::reflect<T>::index_of_method(name)          name → index (optional)
//   psio::reflect<T>::find_method_index(name)        name → index, method_count if absent
//   psio::reflect<T>::index_of_method_hash<Hash>(key)  Hash{}(name) == key → index
//   psio::reflect<T>::for_each_method(fn)            fn(integral_constant<I>, name, pointer)
//   psio::reflect<T>::invoke_method<I>(obj, tuple)   call method I with a tuple of arguments
//
// The proxy:
//
//   psio::reflect<T>::proxy<P>   inherits P (`using P::P`, plus construction
//                                from P and from whatever builds P). Every
//                                field is a member `name()` that returns
//                                P::get<I, &T::name>(); every method is a
//                                member `name(args...)` that returns
//                                P::call<I, &T::name>(args...). The index and
//                                the member pointer are template VALUE
//                                arguments. P needs only the one it is used
//                                for: a field projection defines `get`, a call
//                                dispatcher defines
//                                  template <std::size_t I, auto MemberPtr, typename... Args>
//                                  auto call(Args&&... args);
//
// Population: `PSIO_REFLECT(Type, item...)` — at namespace scope right after
// the type (in any namespace, with an unqualified type name), or inside the
// class body:
//
//     namespace chain {
//        class token {
//           std::uint64_t supply_;                      // private is fine in-class
//           PSIO_REFLECT(token, supply_, method(transfer, from, to, amount))
//           void transfer(account from, account to, std::uint64_t amount);
//        };
//     }
//
// In-class reflection sees private members and members declared after the
// macro. At namespace scope every listed member must be accessible (public).
//
// `attr(field, spec)` and the type-level keywords (definitionWillNotChange(),
// maxFields(N), maxDynamicData(N), transparent(), invariant(fn)) work in
// every PSIO_REFLECT: at namespace
// scope and in a class body, for a type in any (nested) namespace. The
// annotations are members of the metadata the macro binds to the type, and
// `psio::annotate<&T::field>` / `psio::annotate<psio::type<T>{}>` read them
// from there (annotate.hpp), so everything about a type is stated once,
// next to it:
//
//     namespace chain::state {
//        struct account {
//           std::uint64_t id;
//           std::string   owner;
//           PSIO_REFLECT(account, attr(id, json_string), attr(owner, max<32>),
//                        definitionWillNotChange(), maxDynamicData(64))
//        };
//     }
//
// The spec expression of `attr` is evaluated with `using namespace ::psio`
// in effect, inside the metadata (in a class body: after the class's own
// names). PSIO_FIELD_ATTRS / PSIO_TYPE_ATTRS (annotate.hpp) remain as
// separate namespace-scope statements; one spec type declared both ways for
// the same field or type is a compile error.
//
// A method is named by `&T::name`, so an overloaded member function cannot
// be reflected (the address is ambiguous): give the overloads distinct
// names, or reflect a non-overloaded forwarding member. Member function
// templates and ref-qualified member functions are likewise not supported.
// A name may be listed once: a field and a method with one name, or a
// member named like one of the proxy's own members (`psio_get_proxy`,
// `_psio_record_proxy`), is a compile error.
//
// Dispatch: `psio::reflect<T>` inherits the metadata PSIO_REFLECT bound to
// T (see detail::reflect_slot), or the result of a hand-written ADL
// `psio_reflect_helper(T*)`; unreflected types get an empty default with
// is_reflected = false.
//
// Lookup. `index_of`, `find_index` and `visit_field_by_name` resolve a name
// through a perfect hash generated at compile time from the member names
// (name_lookup.hpp): a fixed number of loads and multiplies and exactly one
// confirming compare, whatever the member count, with no run-time
// initialization. `index_of_field_number` / `visit_field_by_number` use a
// direct table. `index_of_hash<Hash>(key)` looks a member up by a 64-bit key
// the caller computed with its own constexpr hash (`Hash{}(std::string_view)`);
// two members with the same key under that hash are a compile error. The
// tables are instantiated only for types that are actually looked up.

#include <psio/error.hpp>
#include <psio/name_lookup.hpp>

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

      // Names a member in an overload set: PSIO_REFLECT generates one
      // `_psio_member_annotations_(member_key<&T::field>)` per `attr`.
      template <auto MemberPtr>
      struct member_key
      {
      };

      // Empty records still expose the metadata template names so generic
      // pack-expansion walkers can look them up. No index is valid, and an
      // attempt to instantiate one remains an error.
      template <std::size_t>
      struct empty_reflected_member;
   }

   // Fallback ADL hook: returns the default empty impl for unreflected
   // types. A hand-written `psio_reflect_helper(T*)` overload in T's
   // namespace is still honoured; PSIO_REFLECT itself registers through
   // the binding below, which works at namespace scope and in a class body.
   template <typename T>
   constexpr detail::reflect_default_impl psio_reflect_helper(T*) noexcept
   {
      return {};
   }

   namespace detail {
      // Registration. `reflect_slot<T>` declares a friend function that
      // `reflect_bind<T, Impl>` defines; instantiating the binding (the
      // macro does it with a static_assert, legal at namespace and at class
      // scope, in any namespace) makes `Impl` reachable from T alone. The
      // lookup does not depend on access, so the macro may sit in a private
      // section of the class it reflects.
      template <typename T>
      struct reflect_slot
      {
         friend constexpr auto psio_reflect_lookup(reflect_slot);
      };
      template <typename T, typename Impl>
      struct reflect_bind
      {
         friend constexpr auto psio_reflect_lookup(reflect_slot<T>) { return Impl{}; }
      };

      template <typename T, typename = void>
      struct reflect_base
      {
         using type = decltype(psio_reflect_helper(static_cast<T*>(nullptr)));
      };
      template <typename T>
      struct reflect_base<T, std::void_t<decltype(psio_reflect_lookup(reflect_slot<T>{}))>>
      {
         using type = decltype(psio_reflect_lookup(reflect_slot<T>{}));
      };
   }

   // Public facade. Inherits from the metadata PSIO_REFLECT registered for
   // T, or from detail::reflect_default_impl for unreflected types.
   template <typename T>
   struct reflect : detail::reflect_base<T>::type
   {
   };

   // Concept gate.
   template <typename T>
   concept Reflected = reflect<T>::is_reflected;

   // ── Function-type decomposition ────────────────────────────────────────
   //
   // method_traits<F> for a pointer to a member function (const or not,
   // noexcept or not) or a pointer to a function (a static member):
   //
   //   result          the declared return type
   //   args            std::tuple<Args...> exactly as declared
   //   params          std::tuple<std::remove_cvref_t<Args>...>
   //   class_type      the class (void for a static / free function)
   //   arity, is_static, is_const
   template <typename F>
   struct method_traits;

   namespace detail {
      template <typename R, typename C, bool Static, bool Const, typename... Args>
      struct method_traits_impl
      {
         using result     = R;
         using args       = std::tuple<Args...>;
         using params     = std::tuple<std::remove_cvref_t<Args>...>;
         using class_type = C;
         static constexpr std::size_t arity     = sizeof...(Args);
         static constexpr bool        is_static = Static;
         static constexpr bool        is_const  = Const;
      };
   }
   template <typename R, typename... Args>
   struct method_traits<R (*)(Args...)> : detail::method_traits_impl<R, void, true, false, Args...> {};
   template <typename R, typename... Args>
   struct method_traits<R (*)(Args...) noexcept> : method_traits<R (*)(Args...)> {};
   template <typename R, typename C, typename... Args>
   struct method_traits<R (C::*)(Args...)> : detail::method_traits_impl<R, C, false, false, Args...> {};
   template <typename R, typename C, typename... Args>
   struct method_traits<R (C::*)(Args...) noexcept> : method_traits<R (C::*)(Args...)> {};
   template <typename R, typename C, typename... Args>
   struct method_traits<R (C::*)(Args...) const> : detail::method_traits_impl<R, C, false, true, Args...> {};
   template <typename R, typename C, typename... Args>
   struct method_traits<R (C::*)(Args...) const noexcept> : method_traits<R (C::*)(Args...) const> {};

   namespace detail {
      // Holder::method_names as name_lookup.hpp's tables want it.
      template <typename Holder>
      struct method_names_holder
      {
         static constexpr auto& _names_array_ = Holder::method_names;
      };

      template <typename Holder, std::size_t I>
      constexpr auto method_param_name_array() noexcept
      {
         constexpr auto& list = Holder::method_param_name_lists[I];
         std::array<std::string_view, list.size()> out{};
         std::size_t                                 at = 0;
         for (const char* p : list)
            out[at++] = std::string_view(p);
         return out;
      }

      template <typename Holder, typename F, std::size_t... Is>
      constexpr void for_each_method_impl(F&& fn, std::index_sequence<Is...>)
      {
         (std::invoke(fn, std::integral_constant<std::size_t, Is>{}, Holder::method_names[Is],
                      Holder::template method_pointer<Is>),
          ...);
      }

      template <typename Ptr, typename Obj, typename Tuple>
      constexpr decltype(auto) invoke_reflected(Ptr ptr, Obj&& obj, Tuple&& args)
      {
         if constexpr (std::is_member_function_pointer_v<Ptr>)
            return std::apply(
               [&](auto&&... a) -> decltype(auto) {
                  return std::invoke(ptr, std::forward<Obj>(obj),
                                     std::forward<decltype(a)>(a)...);
               },
               std::forward<Tuple>(args));
         else
            return std::apply(ptr, std::forward<Tuple>(args));
      }

      // A projection / dispatcher built from arbitrary arguments: its
      // constructor when it has a matching one, its aggregate members
      // otherwise.
      template <typename P, typename... A>
      concept projection_from =
         std::is_constructible_v<P, A...> || requires { P{std::declval<A>()...}; };
      template <typename P, typename... A>
      constexpr P make_projection(A&&... a)
      {
         if constexpr (std::is_constructible_v<P, A...>)
            return P(std::forward<A>(a)...);
         else
            return P{std::forward<A>(a)...};
      }

      // No name is listed twice (a field and a method may not share one)
      // and none is a name the proxy itself declares.
      template <std::size_t N, std::size_t M>
      constexpr bool reflected_names_distinct(const std::array<std::string_view, N>& fields,
                                              const std::array<std::string_view, M>& methods)
      {
         for (std::size_t i = 0; i < N + M; ++i)
            for (std::size_t j = i + 1; j < N + M; ++j)
               if ((i < N ? fields[i] : methods[i - N]) == (j < N ? fields[j] : methods[j - N]))
                  return false;
         return true;
      }
      constexpr bool reflected_name_reserved(std::string_view name)
      {
         return name == "psio_get_proxy" || name == "_psio_record_proxy";
      }
      template <std::size_t N, std::size_t M>
      constexpr bool reflected_names_unreserved(const std::array<std::string_view, N>& fields,
                                                const std::array<std::string_view, M>& methods)
      {
         for (auto n : fields)
            if (reflected_name_reserved(n))
               return false;
         for (auto n : methods)
            if (reflected_name_reserved(n))
               return false;
         return true;
      }
   }

}  // namespace psio

// ── Macro implementation ─────────────────────────────────────────────────
//
// Named accessors forward into a format-specific projection; no field storage is copied.
// `psio_T_` is the reflected type: the generated metadata is a class template
// instantiated with it, so every use below is dependent and is looked up when
// reflect<T> is first used (the class is complete by then, whichever way round
// the members and the macro were written).
#define PSIO_REFLECT_PROXY_FIELD_(r, _, I, FIELD) \
   decltype(auto) FIELD() { return this->psio_get_proxy().template get<I, &psio_T_::FIELD>(); } \
   decltype(auto) FIELD() const { return this->psio_get_proxy().template get<I, &psio_T_::FIELD>(); }

// A method forwards to Projection::call<I, &T::method>(args...): the index and
// the member pointer are template VALUE arguments.
#define PSIO_REFLECT_PROXY_METHOD_(r, _, I, kt) \
   template <typename... psio_A_> \
   decltype(auto) BOOST_PP_TUPLE_ELEM(1, kt)(psio_A_&&... psio_a_) { \
      return static_cast<Projection&>(*this).template call<I, &psio_T_::BOOST_PP_TUPLE_ELEM(1, kt)>( \
         ::std::forward<psio_A_>(psio_a_)...); \
   } \
   template <typename... psio_A_> \
   decltype(auto) BOOST_PP_TUPLE_ELEM(1, kt)(psio_A_&&... psio_a_) const { \
      return static_cast<const Projection&>(*this).template call<I, &psio_T_::BOOST_PP_TUPLE_ELEM(1, kt)>( \
         ::std::forward<psio_A_>(psio_a_)...); \
   }

// SEQ_FOR_EACH_I that tolerates an empty sequence.
#define PSIO_PP_NOTHING_(...)
#define PSIO_PP_SEQ_FOR_EACH_I_(macro, data, seq) \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(seq), PSIO_PP_NOTHING_, BOOST_PP_SEQ_FOR_EACH_I)(macro, data, seq)

// ONE proxy per reflected type: every field as `name()` -> get<I, &T::name>()
// and every method as `name(args...)` -> call<I, &T::name>(args...). The proxy
// inherits the projection / dispatcher and is constructible from whatever
// constructs it (its constructors, or its aggregate members).
#define PSIO_REFLECT_PROXY_(FSEQ, MSEQ) \
   template <typename Projection> \
   struct _psio_record_proxy : Projection { \
      using Projection::Projection; \
      _psio_record_proxy() = default; \
      explicit _psio_record_proxy(Projection p) : Projection(::std::move(p)) {} \
      template <typename psio_A0_, typename... psio_A_> \
         requires(!(sizeof...(psio_A_) == 0 && \
                    ::std::is_base_of_v<Projection, ::std::remove_cvref_t<psio_A0_>>) && \
                  ::psio::detail::projection_from<Projection, psio_A0_, psio_A_...>) \
      explicit(sizeof...(psio_A_) == 0) _psio_record_proxy(psio_A0_&& a0_, psio_A_&&... a_) \
         : Projection(::psio::detail::make_projection<Projection>( \
              ::std::forward<psio_A0_>(a0_), ::std::forward<psio_A_>(a_)...)) {} \
      Projection& psio_get_proxy() { return *this; } \
      const Projection& psio_get_proxy() const { return *this; } \
      PSIO_PP_SEQ_FOR_EACH_I_(PSIO_REFLECT_PROXY_FIELD_, _, FSEQ) \
      PSIO_PP_SEQ_FOR_EACH_I_(PSIO_REFLECT_PROXY_METHOD_, _, MSEQ) \
   }; \
   template <typename Projection> using proxy = _psio_record_proxy<Projection>;

// Each per-field helper takes (r, data, I, FIELD).

#define PSIO_REFLECT_MEMBER_POINTER_CASE_(r, _, I, FIELD) \
   if constexpr (Idx == (I)) return &psio_T_::FIELD;

#define PSIO_REFLECT_MEMBER_NAME_CASE_(r, _, I, FIELD) \
   if constexpr (Idx == (I)) return ::std::string_view(BOOST_PP_STRINGIZE(FIELD));

#define PSIO_REFLECT_MEMBER_TYPE_CASE_(r, _, I, FIELD)                                       \
   if constexpr (Idx == (I))                                                                  \
      return ::std::type_identity<                                                            \
         ::std::remove_reference_t<decltype(::std::declval<psio_T_&>().FIELD)>>{};

#define PSIO_REFLECT_NAME_ARRAY_ELEM_(r, _, I, FIELD) \
   BOOST_PP_COMMA_IF(I) ::std::string_view(BOOST_PP_STRINGIZE(FIELD))

// One `case` of the switch over a member index.
#define PSIO_REFLECT_VISIT_INDEX_CASE_(r, _, I, FIELD)   \
   case (I):                                              \
      ::std::invoke(::std::forward<F>(fn), obj.FIELD);    \
      return true;

#define PSIO_REFLECT_NUMBER_ARRAY_ELEM_(r, _, I, FIELD) \
   BOOST_PP_COMMA_IF(I) field_number<(I)>

#define PSIO_REFLECT_FOR_EACH_CASE_(r, _, I, FIELD)              \
   ::std::invoke(::std::forward<F>(fn),                           \
                 ::std::integral_constant<::std::size_t, (I)>{},  \
                 ::std::string_view(BOOST_PP_STRINGIZE(FIELD)),   \
                 obj.FIELD);

// Each per-method helper takes (r, data, I, kt), kt = (method, NAME, params...).
#define PSIO_REFLECT_METHOD_POINTER_CASE_(r, _, I, kt) \
   if constexpr (Idx == (I)) return &psio_T_::BOOST_PP_TUPLE_ELEM(1, kt);

#define PSIO_REFLECT_METHOD_NAME_ELEM_(r, _, I, kt) \
   BOOST_PP_COMMA_IF(I) ::std::string_view(BOOST_PP_STRINGIZE(BOOST_PP_TUPLE_ELEM(1, kt)))

// "p1", "p2", ... (trailing comma; nothing for no parameters).
#define PSIO_PP_QUOTE_EACH_(...) \
   __VA_OPT__(PSIO_PP_RESCAN2_(PSIO_PP_QUOTE_STEP_(__VA_ARGS__)))
#define PSIO_PP_QUOTE_STEP_(a, ...) \
   BOOST_PP_STRINGIZE(a), __VA_OPT__(PSIO_PP_QUOTE_AGAIN_ PSIO_PP_PARENS_(__VA_ARGS__))
#define PSIO_PP_QUOTE_AGAIN_() PSIO_PP_QUOTE_STEP_
#define PSIO_REFLECT_METHOD_PARAMS_(kind, name, ...) PSIO_PP_QUOTE_EACH_(__VA_ARGS__)
#define PSIO_REFLECT_METHOD_PARAM_LIST_(r, _, I, kt) \
   ::std::initializer_list<const char*>{PSIO_REFLECT_METHOD_PARAMS_ kt},

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
// PSIO_REFLECT accepts six kinds of arguments:
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
//   6. method(name, param1, param2, ...)
//      → registered as a method: an instance member function (const or
//      not), or a static member function. The parameter names are optional
//      metadata (WIT / schema parameter names); list none, or one per
//      parameter.
//
//   7. transparent()                  type-level keyword
//      → emits the type-level `psio::transparent_spec` annotation: the
//      type has exactly one data member and IS that member in every core
//      format (psio/transparent.hpp).
//
//   8. invariant(fn)                  type-level keyword
//      → the type's semantic invariant (psio/invariant.hpp): `fn` is any
//      expression callable as `fn(const T&) noexcept` returning
//      `psio::codec_status`. In a class body it may name a static member
//      declared anywhere in the class; at namespace scope it is looked up
//      where the macro is written. At most one per type.
//
// All type-level keywords (3-5, 7) are aggregated into one
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
// The keywords are recognised by token pasting only: no macro named `attr`,
// `method`, `definitionWillNotChange`, `maxFields`, `maxDynamicData`,
// `transparent` or `invariant` is defined, so they do not collide with user
// identifiers.

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
// method(name, param1, param2, ...) → (method, name, param1, param2, ...), 1
#define PSIO_REFLECT_KW_method(...) (method, __VA_ARGS__), 1
// transparent() → (typeattr_transparent), 1
#define PSIO_REFLECT_KW_transparent(...) (typeattr_transparent), 1
// invariant(fn) → (typeattr_invariant, fn), 1
#define PSIO_REFLECT_KW_invariant(...) (typeattr_invariant, __VA_ARGS__), 1

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
#define PSIO_REFLECT_KIND_method                 5
#define PSIO_REFLECT_KIND_typeattr_transparent   6
#define PSIO_REFLECT_KIND_typeattr_invariant     7
#define PSIO_REFLECT_KEEP_METHOD(s, _, kt) \
   BOOST_PP_EQUAL(5, BOOST_PP_CAT(PSIO_REFLECT_KIND_, BOOST_PP_TUPLE_ELEM(0, kt)))

// Field-name extractor.  For `(field, F)` and `(attrfield, F, ...)`
// the name is element 1 of the tuple.
#define PSIO_REFLECT_NAME_OF(s, _, kt) BOOST_PP_TUPLE_ELEM(1, kt)

// ── Annotations carried by the metadata ───────────────────────────────────
//
// SEQ_FOR_EACH that tolerates an empty sequence.
#define PSIO_PP_SEQ_FOR_EACH_(macro, data, seq) \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(seq), PSIO_PP_NOTHING_, BOOST_PP_SEQ_FOR_EACH)(macro, data, seq)
// (a, b, rest...) -> rest...: the spec expression of an attrfield tuple,
// commas included.
#define PSIO_PP_REST2_(a, b, ...) __VA_ARGS__

// Field-level. FKSEQ is the classified field list — `(field, F)` and
// `(attrfield, F, spec...)`. Each attrfield becomes one overload of
// `_psio_member_annotations_`, selected by the member pointer through
// detail::member_key; plain fields emit nothing, and a type with no attr
// has no such function at all (has_member_annotations says which).
#define PSIO_REFLECT_MEMBER_ANN_FN_(r, _, kt) \
   BOOST_PP_CAT(PSIO_REFLECT_MEMBER_ANN_FN_, BOOST_PP_TUPLE_ELEM(0, kt))(kt)
#define PSIO_REFLECT_MEMBER_ANN_FN_field(kt) /**/
#define PSIO_REFLECT_MEMBER_ANN_FN_attrfield(kt)                                          \
      static constexpr auto _psio_member_annotations_(                                    \
         ::psio::detail::member_key<&psio_T_::BOOST_PP_TUPLE_ELEM(1, kt)>) noexcept       \
      {                                                                                   \
         using namespace ::psio;                                                          \
         return ::psio::to_spec_tuple(PSIO_PP_REST2_ kt);                                 \
      }

// Presence markers: a token per attrfield / per type-level keyword, so
// CHECK_EMPTY tells whether the list has any.
#define PSIO_REFLECT_ATTR_PRESENT(s, _, kt) \
   BOOST_PP_CAT(PSIO_REFLECT_ATTR_PRESENT_, BOOST_PP_TUPLE_ELEM(0, kt))
#define PSIO_REFLECT_ATTR_PRESENT_field     /**/
#define PSIO_REFLECT_ATTR_PRESENT_attrfield 1

#define PSIO_REFLECT_MEMBER_ANN_(FKSEQ)                                                    \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(                                                      \
                   PSIO_PP_SEQ_FOR_EACH_(PSIO_REFLECT_ATTR_PRESENT, _, FKSEQ)),            \
                PSIO_REFLECT_MEMBER_ANN_NONE_, PSIO_REFLECT_MEMBER_ANN_SOME_)(FKSEQ)
#define PSIO_REFLECT_MEMBER_ANN_NONE_(FKSEQ) \
      static constexpr bool has_member_annotations = false;
#define PSIO_REFLECT_MEMBER_ANN_SOME_(FKSEQ)                                               \
      static constexpr bool has_member_annotations = true;                                 \
      BOOST_PP_SEQ_FOR_EACH(PSIO_REFLECT_MEMBER_ANN_FN_, _, FKSEQ)

// Type-level: `, std::tuple{spec}` per keyword, concatenated into one tuple
// so `definitionWillNotChange(), maxFields(5), maxDynamicData(4096)` compose.
#define PSIO_REFLECT_TYPEATTR_TUPLE(s, _, kt)                                  \
   BOOST_PP_CAT(PSIO_REFLECT_TYPEATTR_TUPLE_, BOOST_PP_TUPLE_ELEM(0, kt))(kt)
#define PSIO_REFLECT_TYPEATTR_TUPLE_field(kt)                                  /**/
#define PSIO_REFLECT_TYPEATTR_TUPLE_attrfield(kt)                              /**/
#define PSIO_REFLECT_TYPEATTR_TUPLE_method(kt)                                 /**/
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_dwnc(kt)                          \
   , ::std::tuple{::psio::definition_will_not_change{}}
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_max_fields(kt)                    \
   , ::std::tuple{::psio::max_fields_spec{BOOST_PP_TUPLE_ELEM(1, kt)}}
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_max_dyn_data(kt)                  \
   , ::std::tuple{::psio::max_dynamic_data_spec{BOOST_PP_TUPLE_ELEM(1, kt)}}

#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_transparent(kt)                   \
   , ::std::tuple{::psio::transparent_spec{}}
#define PSIO_REFLECT_TYPEATTR_TUPLE_typeattr_invariant(kt)                     /**/

#define PSIO_REFLECT_TYPEATTR_PRESENT(s, _, kt)                                \
   BOOST_PP_CAT(PSIO_REFLECT_TYPEATTR_PRESENT_, BOOST_PP_TUPLE_ELEM(0, kt))
#define PSIO_REFLECT_TYPEATTR_PRESENT_field                                    /**/
#define PSIO_REFLECT_TYPEATTR_PRESENT_attrfield                                /**/
#define PSIO_REFLECT_TYPEATTR_PRESENT_method                                   /**/
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_dwnc          1
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_max_fields    1
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_max_dyn_data  1
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_transparent   1
#define PSIO_REFLECT_TYPEATTR_PRESENT_typeattr_invariant     /**/

#define PSIO_REFLECT_TYPE_ANN_(TYPE, KIND_SEQ)                                             \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(                                                      \
                   PSIO_PP_SEQ_FOR_EACH_(PSIO_REFLECT_TYPEATTR_PRESENT, _, KIND_SEQ)),     \
                PSIO_REFLECT_TYPE_ANN_NONE_, PSIO_REFLECT_TYPE_ANN_SOME_)(TYPE, KIND_SEQ)
#define PSIO_REFLECT_TYPE_ANN_NONE_(TYPE, KIND_SEQ) \
      static constexpr ::std::tuple<> type_annotations{};
#define PSIO_REFLECT_TYPE_ANN_SOME_(TYPE, KIND_SEQ)                                        \
      static constexpr auto type_annotations = ::std::tuple_cat(                           \
         ::std::tuple<>{} BOOST_PP_SEQ_FOR_EACH(PSIO_REFLECT_TYPEATTR_TUPLE, _, KIND_SEQ));

// The invariant: `invariant(fn)` becomes one static member function of the
// metadata, `_psio_invariant_(integral_constant<I>, const T&)` where I is the
// keyword's position in the argument list, so that a second `invariant(...)`
// is a distinct overload and the count below gives the diagnostic.
// psio/invariant.hpp reads it (has_invariant_v, check_invariant). The call
// is in a member function body: in a class body that is a complete-class
// context, so `fn` may name a static member declared after the macro.
#define PSIO_PP_REST1_(a, ...) __VA_ARGS__
#define PSIO_REFLECT_INVARIANT_FN_(r, TYPE, I, kt) \
   BOOST_PP_CAT(PSIO_REFLECT_INVARIANT_FN_, BOOST_PP_TUPLE_ELEM(0, kt))(TYPE, I, kt)
#define PSIO_REFLECT_INVARIANT_FN_field(TYPE, I, kt)                  /**/
#define PSIO_REFLECT_INVARIANT_FN_attrfield(TYPE, I, kt)              /**/
#define PSIO_REFLECT_INVARIANT_FN_method(TYPE, I, kt)                 /**/
#define PSIO_REFLECT_INVARIANT_FN_typeattr_dwnc(TYPE, I, kt)          /**/
#define PSIO_REFLECT_INVARIANT_FN_typeattr_max_fields(TYPE, I, kt)    /**/
#define PSIO_REFLECT_INVARIANT_FN_typeattr_max_dyn_data(TYPE, I, kt)  /**/
#define PSIO_REFLECT_INVARIANT_FN_typeattr_transparent(TYPE, I, kt)   /**/
#define PSIO_REFLECT_INVARIANT_FN_typeattr_invariant(TYPE, I, kt)                          \
      static ::psio::codec_status _psio_invariant_(                                        \
         ::std::integral_constant<::std::size_t, I>, const psio_T_& psio_v_) noexcept      \
      {                                                                                    \
         constexpr bool psio_ok_ = requires {                                              \
            { (PSIO_PP_REST1_ kt)(psio_v_) } noexcept                                      \
               -> ::std::same_as<::psio::codec_status>;                                    \
         };                                                                                \
         static_assert(psio_ok_,                                                           \
                       "psio: invariant(fn) in PSIO_REFLECT(" BOOST_PP_STRINGIZE(TYPE)     \
                       ") needs fn callable as fn(const T&), declared noexcept and "       \
                       "returning psio::codec_status");                                    \
         if constexpr (psio_ok_)                                                           \
            return (PSIO_PP_REST1_ kt)(psio_v_);                                           \
         else                                                                              \
            return {};                                                                     \
      }
#define PSIO_REFLECT_INVARIANT_ONE_(r, _, I, kt) \
   BOOST_PP_CAT(PSIO_REFLECT_INVARIANT_ONE_, BOOST_PP_TUPLE_ELEM(0, kt))(I)
#define PSIO_REFLECT_INVARIANT_ONE_field(I)                  /**/
#define PSIO_REFLECT_INVARIANT_ONE_attrfield(I)              /**/
#define PSIO_REFLECT_INVARIANT_ONE_method(I)                 /**/
#define PSIO_REFLECT_INVARIANT_ONE_typeattr_dwnc(I)          /**/
#define PSIO_REFLECT_INVARIANT_ONE_typeattr_max_fields(I)    /**/
#define PSIO_REFLECT_INVARIANT_ONE_typeattr_max_dyn_data(I)  /**/
#define PSIO_REFLECT_INVARIANT_ONE_typeattr_transparent(I)   /**/
#define PSIO_REFLECT_INVARIANT_ONE_typeattr_invariant(I)     +1
#define PSIO_REFLECT_INVARIANT_AT_(r, _, I, kt) \
   BOOST_PP_CAT(PSIO_REFLECT_INVARIANT_AT_, BOOST_PP_TUPLE_ELEM(0, kt))(I)
#define PSIO_REFLECT_INVARIANT_AT_field(I)                  /**/
#define PSIO_REFLECT_INVARIANT_AT_attrfield(I)              /**/
#define PSIO_REFLECT_INVARIANT_AT_method(I)                 /**/
#define PSIO_REFLECT_INVARIANT_AT_typeattr_dwnc(I)          /**/
#define PSIO_REFLECT_INVARIANT_AT_typeattr_max_fields(I)    /**/
#define PSIO_REFLECT_INVARIANT_AT_typeattr_max_dyn_data(I)  /**/
#define PSIO_REFLECT_INVARIANT_AT_typeattr_transparent(I)   /**/
#define PSIO_REFLECT_INVARIANT_AT_typeattr_invariant(I)     +(I)
#define PSIO_REFLECT_INVARIANT_(TYPE, KIND_SEQ)                                            \
      static constexpr ::std::size_t _psio_invariant_count_ =                              \
         0 PSIO_PP_SEQ_FOR_EACH_I_(PSIO_REFLECT_INVARIANT_ONE_, _, KIND_SEQ);              \
      static constexpr ::std::size_t _psio_invariant_index_ =                              \
         0 PSIO_PP_SEQ_FOR_EACH_I_(PSIO_REFLECT_INVARIANT_AT_, _, KIND_SEQ);               \
      static_assert(_psio_invariant_count_ <= 1,                                           \
                    "psio: PSIO_REFLECT(" BOOST_PP_STRINGIZE(TYPE)                         \
                    ") declares invariant(...) twice: a type has one invariant");          \
      PSIO_PP_SEQ_FOR_EACH_I_(PSIO_REFLECT_INVARIANT_FN_, TYPE, KIND_SEQ)

// PSIO_REFLECT(TYPE)             — zero-field marker (resource opt-in)
// PSIO_REFLECT(TYPE, f1, f2, …)  — record / struct
//
// Dispatch on __VA_OPT__: BOOST_PP_VARIADIC_TO_SEQ on truly-empty input
// produces a one-element seq with empty content, which then trips the
// downstream macros. Splitting on emptiness avoids the issue.
#define PSIO_REFLECT(TYPE, ...) \
   BOOST_PP_CAT(PSIO_REFLECT_DISPATCH_, \
                BOOST_PP_CAT(0, __VA_OPT__(_HAS)))(TYPE, __VA_ARGS__)

// (a, b, c) -> (a)(b)(c) by __VA_OPT__ recursion. BOOST_PP_VARIADIC_TO_SEQ
// stops at 64 arguments (BOOST_PP_LIMIT_VARIADIC); this one is bounded only
// by the rescans below (4^4 = 256), which is also Boost's sequence limit, so
// a record may list up to 255 members and keywords.
#define PSIO_PP_PARENS_ ()
#define PSIO_PP_RESCAN_(...) \
   PSIO_PP_RESCAN1_(PSIO_PP_RESCAN1_(PSIO_PP_RESCAN1_(PSIO_PP_RESCAN1_(__VA_ARGS__))))
#define PSIO_PP_RESCAN1_(...) \
   PSIO_PP_RESCAN2_(PSIO_PP_RESCAN2_(PSIO_PP_RESCAN2_(PSIO_PP_RESCAN2_(__VA_ARGS__))))
#define PSIO_PP_RESCAN2_(...) \
   PSIO_PP_RESCAN3_(PSIO_PP_RESCAN3_(PSIO_PP_RESCAN3_(PSIO_PP_RESCAN3_(__VA_ARGS__))))
#define PSIO_PP_RESCAN3_(...) \
   PSIO_PP_RESCAN4_(PSIO_PP_RESCAN4_(PSIO_PP_RESCAN4_(PSIO_PP_RESCAN4_(__VA_ARGS__))))
#define PSIO_PP_RESCAN4_(...) __VA_ARGS__
#define PSIO_PP_VARIADIC_TO_SEQ(...) \
   __VA_OPT__(PSIO_PP_RESCAN_(PSIO_PP_VARIADIC_TO_SEQ_STEP_(__VA_ARGS__)))
#define PSIO_PP_VARIADIC_TO_SEQ_STEP_(a, ...) \
   (a) __VA_OPT__(PSIO_PP_VARIADIC_TO_SEQ_AGAIN_ PSIO_PP_PARENS_(__VA_ARGS__))
#define PSIO_PP_VARIADIC_TO_SEQ_AGAIN_() PSIO_PP_VARIADIC_TO_SEQ_STEP_

#define PSIO_REFLECT_DISPATCH_0(TYPE, ...) PSIO_REFLECT_IMPL_(TYPE, , , , )
#define PSIO_REFLECT_DISPATCH_0_HAS(TYPE, ...)                                         \
   PSIO_REFLECT_DISPATCH_(                                                              \
      TYPE,                                                                             \
      BOOST_PP_SEQ_TRANSFORM(PSIO_REFLECT_CLASSIFY, _,                                  \
                             PSIO_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))

#define PSIO_REFLECT_FIELD_NAMES_(SEQ) \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(SEQ), PSIO_PP_NOTHING_, BOOST_PP_SEQ_TRANSFORM)( \
      PSIO_REFLECT_NAME_OF, _, SEQ)

// FSEQ: the field names. MSEQ: the method tuples. FKSEQ: the classified
// field tuples (for member annotations). KIND_SEQ: every classified item
// (for the type-level keywords). The field list is filtered once.
#define PSIO_REFLECT_DISPATCH_(TYPE, KIND_SEQ)                                           \
   PSIO_REFLECT_DISPATCH_FIELDS_(                                                        \
      TYPE, BOOST_PP_SEQ_FILTER(PSIO_REFLECT_KEEP_FIELD, _, KIND_SEQ), KIND_SEQ)
#define PSIO_REFLECT_DISPATCH_FIELDS_(TYPE, FKSEQ, KIND_SEQ)                             \
   PSIO_REFLECT_IMPL_(TYPE, PSIO_REFLECT_FIELD_NAMES_(FKSEQ),                            \
                      BOOST_PP_SEQ_FILTER(PSIO_REFLECT_KEEP_METHOD, _, KIND_SEQ), FKSEQ,  \
                      KIND_SEQ)

// Field metadata for a type with no reflected fields — a resource marker
//
//    struct pollable : psio::wit_resource {};
//    PSIO_REFLECT(pollable)
//
// or a class reflected for its methods only. member_count is 0 and the
// visit / for_each entry points short-circuit; the member_pointer /
// member_name / member_type templates exist (generic pack-expansion walkers
// name them) but have no valid index.
#define PSIO_REFLECT_FIELDS_EMPTY_(SEQ)                                                \
      static constexpr ::std::size_t      member_count = 0;                            \
      static constexpr ::std::array<::std::string_view, 0> _names_array_{};            \
      static constexpr ::std::array<::std::uint32_t, 0>    numbers{};                  \
      template <::std::size_t Idx>                                                    \
      using member_type = typename ::psio::detail::empty_reflected_member<Idx>::type; \
      template <::std::size_t Idx>                                                    \
      static constexpr auto member_pointer                                           \
         = ::psio::detail::empty_reflected_member<Idx>::pointer;                       \
      template <::std::size_t Idx>                                                    \
      static constexpr ::std::string_view member_name                                 \
         = ::psio::detail::empty_reflected_member<Idx>::name;                          \
      template <::std::size_t Idx>                                                    \
      static constexpr ::std::uint32_t field_number                                   \
         = ::psio::detail::empty_reflected_member<Idx>::number;                        \
                                                                                       \
      static constexpr ::std::optional<::std::size_t>                                  \
      index_of(::std::string_view) noexcept                                            \
      {                                                                                \
         return ::std::nullopt;                                                        \
      }                                                                                \
                                                                                       \
      static constexpr ::std::size_t find_index(::std::string_view) noexcept          \
      {                                                                                \
         return 0;                                                                     \
      }                                                                                \
                                                                                       \
      template <typename Hash>                                                         \
      static constexpr ::std::optional<::std::size_t>                                  \
      index_of_hash(::std::uint64_t) noexcept                                          \
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
      static constexpr bool visit_field_by_index(Obj&, ::std::size_t, F&&)             \
      {                                                                                \
         return false;                                                                 \
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
      }

#define PSIO_REFLECT_FIELDS_(SEQ)                                                       \
      static constexpr ::std::size_t      member_count = BOOST_PP_SEQ_SIZE(SEQ);         \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr auto _member_pointer_impl_() noexcept                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_MEMBER_POINTER_CASE_, _, SEQ)             \
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
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_MEMBER_TYPE_CASE_, _, SEQ)                \
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
      static constexpr ::std::array<::std::uint32_t, member_count> numbers               \
         = {BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_NUMBER_ARRAY_ELEM_, _, SEQ)};           \
                                                                                         \
      /* Index of the member named name_, or member_count. */                            \
      static constexpr ::std::size_t find_index(::std::string_view name_) noexcept       \
      {                                                                                  \
         return ::psio::detail::name_table_for<_psio_self_>.find(name_);                 \
      }                                                                                  \
                                                                                         \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of(::std::string_view name_) noexcept                                        \
      {                                                                                  \
         const ::std::size_t i_ = find_index(name_);                                     \
         if (i_ == member_count) return ::std::nullopt;                                  \
         return i_;                                                                      \
      }                                                                                  \
                                                                                         \
      /* Index of the member whose name hashes to key_ under Hash. */                    \
      template <typename Hash>                                                           \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of_hash(::std::uint64_t key_) noexcept                                       \
      {                                                                                  \
         const ::std::size_t i_ = ::psio::detail::hashed_table_for<                      \
            _psio_self_, Hash>::table.find(key_);                                        \
         if (i_ == member_count) return ::std::nullopt;                                  \
         return i_;                                                                      \
      }                                                                                  \
                                                                                         \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of_field_number(::std::uint32_t n) noexcept                                  \
      {                                                                                  \
         const ::std::size_t i_ = ::psio::detail::number_table_for<                      \
            _psio_self_>.find(n);                                                        \
         if (i_ == member_count) return ::std::nullopt;                                  \
         return i_;                                                                      \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_index(                                        \
         Obj& obj, ::std::size_t index_, F&& fn)                                         \
      {                                                                                  \
         switch (index_)                                                                 \
         {                                                                               \
            BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_VISIT_INDEX_CASE_, _, SEQ)             \
            default: return false;                                                       \
         }                                                                               \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_name(                                         \
         Obj& obj, ::std::string_view lookup_name, F&& fn)                               \
      {                                                                                  \
         return visit_field_by_index(obj, find_index(lookup_name),                       \
                                     ::std::forward<F>(fn));                             \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_number(                                       \
         Obj& obj, ::std::uint32_t lookup_num, F&& fn)                                   \
      {                                                                                  \
         return visit_field_by_index(                                                    \
            obj,                                                                         \
            ::psio::detail::number_table_for<_psio_self_>.find(lookup_num),              \
            ::std::forward<F>(fn));                                                      \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr void for_each_field(Obj& obj, F&& fn)                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_FOR_EACH_CASE_, _, SEQ)                   \
      }

// Method metadata for a type that lists no method(...).
#define PSIO_REFLECT_METHODS_EMPTY_(SEQ)                                               \
      static constexpr ::std::size_t method_count = 0;                                 \
      static constexpr ::std::array<::std::string_view, 0> method_names{};             \
      template <::std::size_t Idx>                                                    \
      using method_type = typename ::psio::detail::empty_reflected_member<Idx>::type; \
      template <::std::size_t Idx>                                                    \
      static constexpr auto method_pointer                                           \
         = ::psio::detail::empty_reflected_member<Idx>::pointer;                       \
      template <::std::size_t Idx>                                                    \
      static constexpr ::std::string_view method_name                                 \
         = ::psio::detail::empty_reflected_member<Idx>::name;                          \
      static constexpr ::std::size_t find_method_index(::std::string_view) noexcept   \
      {                                                                                \
         return 0;                                                                     \
      }                                                                                \
      static constexpr ::std::optional<::std::size_t>                                  \
      index_of_method(::std::string_view) noexcept                                     \
      {                                                                                \
         return ::std::nullopt;                                                        \
      }                                                                                \
      template <typename Hash>                                                         \
      static constexpr ::std::optional<::std::size_t>                                  \
      index_of_method_hash(::std::uint64_t) noexcept                                   \
      {                                                                                \
         return ::std::nullopt;                                                        \
      }                                                                                \
      template <typename F>                                                            \
      static constexpr void for_each_method(F&&)                                       \
      {                                                                                \
      }

#define PSIO_REFLECT_METHODS_(SEQ)                                                      \
      static constexpr ::std::size_t method_count = BOOST_PP_SEQ_SIZE(SEQ);              \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr auto _method_pointer_impl_() noexcept                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_METHOD_POINTER_CASE_, _, SEQ)             \
      }                                                                                  \
      /* &T::method — a member function pointer, or a function pointer for a */          \
      /* static member.                                                      */          \
      template <::std::size_t Idx>                                                       \
      static constexpr auto method_pointer = _method_pointer_impl_<Idx>();               \
      template <::std::size_t Idx>                                                       \
      using method_type = decltype(_method_pointer_impl_<Idx>());                        \
                                                                                         \
      static constexpr ::std::array<::std::string_view, method_count> method_names       \
         = {BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_METHOD_NAME_ELEM_, _, SEQ)};            \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::string_view method_name = method_names[Idx];               \
                                                                                         \
      /* Parameter names as written in method(name, p1, p2, ...); may be */              \
      /* fewer than the method's parameters (names are optional).        */              \
      static constexpr ::std::initializer_list<const char*>                              \
         method_param_name_lists[method_count] = {                                       \
            BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_METHOD_PARAM_LIST_, _, SEQ)};           \
      template <::std::size_t Idx>                                                       \
      static constexpr auto method_param_names                                           \
         = ::psio::detail::method_param_name_array<_psio_self_, Idx>();                  \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      using method_traits = ::psio::method_traits<method_type<Idx>>;                     \
      template <::std::size_t Idx>                                                       \
      using method_result = typename method_traits<Idx>::result;                         \
      template <::std::size_t Idx>                                                       \
      using method_args = typename method_traits<Idx>::args;                             \
      template <::std::size_t Idx>                                                       \
      using method_params = typename method_traits<Idx>::params;                         \
      template <::std::size_t Idx>                                                       \
      static constexpr bool method_is_static = method_traits<Idx>::is_static;            \
      template <::std::size_t Idx>                                                       \
      static constexpr bool method_is_const = method_traits<Idx>::is_const;              \
                                                                                         \
      /* Index of the method named name_, or method_count. */                            \
      static constexpr ::std::size_t find_method_index(::std::string_view name_) noexcept \
      {                                                                                  \
         return ::psio::detail::name_table_for<                                          \
            ::psio::detail::method_names_holder<_psio_self_>>.find(name_);               \
      }                                                                                  \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of_method(::std::string_view name_) noexcept                                 \
      {                                                                                  \
         const ::std::size_t i_ = find_method_index(name_);                              \
         if (i_ == method_count) return ::std::nullopt;                                  \
         return i_;                                                                      \
      }                                                                                  \
      /* Index of the method whose name hashes to key_ under Hash; two */                \
      /* methods with the same key are a compile error.                */                \
      template <typename Hash>                                                           \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of_method_hash(::std::uint64_t key_) noexcept                                \
      {                                                                                  \
         const ::std::size_t i_ = ::psio::detail::hashed_table_for<                      \
            ::psio::detail::method_names_holder<_psio_self_>, Hash>::table.find(key_);   \
         if (i_ == method_count) return ::std::nullopt;                                  \
         return i_;                                                                      \
      }                                                                                  \
      /* fn(std::integral_constant<std::size_t, I>, name, &T::method) per method. */     \
      template <typename F>                                                              \
      static constexpr void for_each_method(F&& fn)                                      \
      {                                                                                  \
         ::psio::detail::for_each_method_impl<_psio_self_>(                              \
            fn, ::std::make_index_sequence<method_count>{});                             \
      }                                                                                  \
      /* Call method Idx on obj with the arguments in a tuple (obj is ignored */         \
      /* for a static method).                                                */         \
      template <::std::size_t Idx, typename Obj, typename Tuple>                         \
      static constexpr decltype(auto) invoke_method(Obj&& obj, Tuple&& args)             \
      {                                                                                  \
         return ::psio::detail::invoke_reflected(                                        \
            method_pointer<Idx>, ::std::forward<Obj>(obj), ::std::forward<Tuple>(args)); \
      }

// The metadata is a class template over the reflected type (see
// PSIO_REFLECT_PROXY_FIELD_), bound to TYPE by instantiating
// detail::reflect_bind in a static_assert — a declaration that is legal at
// namespace scope and inside TYPE's own class body.
#define PSIO_REFLECT_IMPL_(TYPE, FSEQ, MSEQ, FKSEQ, KIND_SEQ)                           \
   template <typename psio_T_>                                                           \
   struct BOOST_PP_CAT(psio_reflect_impl_, TYPE)                                        \
   {                                                                                     \
      using type        = psio_T_;                                                       \
      using _psio_self_ = BOOST_PP_CAT(psio_reflect_impl_, TYPE);                        \
      PSIO_REFLECT_PROXY_(FSEQ, MSEQ)                                                    \
      static constexpr bool               is_reflected = true;                           \
      static constexpr ::std::string_view name         = BOOST_PP_STRINGIZE(TYPE);       \
      BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(FSEQ), PSIO_REFLECT_FIELDS_EMPTY_,               \
                   PSIO_REFLECT_FIELDS_)(FSEQ)                                           \
      BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(MSEQ), PSIO_REFLECT_METHODS_EMPTY_,              \
                   PSIO_REFLECT_METHODS_)(MSEQ)                                          \
      PSIO_REFLECT_MEMBER_ANN_(FKSEQ)                                                    \
      PSIO_REFLECT_TYPE_ANN_(TYPE, KIND_SEQ)                                             \
      PSIO_REFLECT_INVARIANT_(TYPE, KIND_SEQ)                                            \
      static_assert(::psio::detail::reflected_names_distinct(_names_array_, method_names), \
                    "psio: PSIO_REFLECT(" BOOST_PP_STRINGIZE(TYPE)                       \
                    ") lists a name twice (a field and a method may not share a name)"); \
      static_assert(::psio::detail::reflected_names_unreserved(_names_array_, method_names), \
                    "psio: PSIO_REFLECT(" BOOST_PP_STRINGIZE(TYPE)                       \
                    ") lists a member named like a proxy internal "                      \
                    "(psio_get_proxy, _psio_record_proxy)");                             \
   };                                                                                    \
   static_assert(sizeof(::psio::detail::reflect_bind<                                    \
                    TYPE, BOOST_PP_CAT(psio_reflect_impl_, TYPE)<TYPE>>) > 0, "");

// ── Enumerator names ──────────────────────────────────────────────────────
//
// C++ has no enumerator reflection. One line gives an enum its names:
//
//     namespace chain {
//        enum class status : std::uint8_t { pending, active = 2, closed };
//        PSIO_REFLECT_ENUM(status, pending, active, closed)
//     }
//
// usable at namespace scope in any namespace (unqualified enum name) and
// inside a class body for a nested enum. `psio::reflect_enum<E>` then has
//
//   is_reflected            true (false, with nothing else, for other types)
//   name                    "status"
//   count                   number of listed enumerators
//   names / values          std::array<std::string_view, N> / std::array<E, N>
//   name_of(e)              optional<string_view>; nullopt for a value that
//                           is not a listed enumerator (the first listed name
//                           when two enumerators share a value)
//   value_of(name)          optional<E>
//
// The text formats use the names (json.hpp: an enumerator is written as its
// name, any other value as its number); binary formats are unaffected — an
// enum is its underlying integer. An enum without the declaration is its
// number in text too.

namespace psio
{
   namespace detail
   {
      struct reflect_enum_default
      {
         static constexpr bool is_reflected = false;
      };
      template <typename E>
      struct enum_slot
      {
         friend constexpr auto psio_enum_lookup(enum_slot);
      };
      template <typename E, typename Impl>
      struct enum_bind
      {
         friend constexpr auto psio_enum_lookup(enum_slot<E>) { return Impl{}; }
      };
      template <typename E, typename = void>
      struct enum_base
      {
         using type = reflect_enum_default;
      };
      template <typename E>
      struct enum_base<E, std::void_t<decltype(psio_enum_lookup(enum_slot<E>{}))>>
      {
         using type = decltype(psio_enum_lookup(enum_slot<E>{}));
      };

      // The listed values are 0, 1, ... in order: name_of is an index.
      template <typename E, std::size_t N>
      constexpr bool enum_values_dense(const std::array<E, N>& values) noexcept
      {
         for (std::size_t i = 0; i < N; ++i)
            if (static_cast<std::underlying_type_t<E>>(values[i]) !=
                static_cast<std::underlying_type_t<E>>(i))
               return false;
         return true;
      }

      template <typename Impl>
      struct reflect_enum_api : Impl
      {
         using enum_type = typename Impl::type;
         static constexpr bool dense = enum_values_dense(Impl::values);

         static constexpr std::optional<std::string_view> name_of(enum_type value) noexcept
         {
            using U = std::underlying_type_t<enum_type>;
            if constexpr (dense)
            {
               const U raw = static_cast<U>(value);
               if (raw >= 0 && static_cast<std::make_unsigned_t<U>>(raw) < Impl::count)
                  return Impl::names[static_cast<std::size_t>(raw)];
               return std::nullopt;
            }
            else
            {
               for (std::size_t i = 0; i < Impl::count; ++i)
                  if (Impl::values[i] == value)
                     return Impl::names[i];
               return std::nullopt;
            }
         }
         static constexpr std::optional<enum_type> value_of(std::string_view name) noexcept
         {
            const std::size_t i = name_table_for<Impl>.find(name);
            if (i == Impl::count)
               return std::nullopt;
            return Impl::values[i];
         }
      };
      template <>
      struct reflect_enum_api<reflect_enum_default> : reflect_enum_default
      {
      };
   }  // namespace detail

   template <typename E>
   struct reflect_enum : detail::reflect_enum_api<typename detail::enum_base<E>::type>
   {
   };

   template <typename E>
   concept ReflectedEnum = std::is_enum_v<E> && reflect_enum<E>::is_reflected;
}  // namespace psio

#define PSIO_REFLECT_ENUM_NAME_(r, _, I, NAME) \
   BOOST_PP_COMMA_IF(I) ::std::string_view(BOOST_PP_STRINGIZE(NAME))
#define PSIO_REFLECT_ENUM_VALUE_(r, ENUM, I, NAME) BOOST_PP_COMMA_IF(I) ENUM::NAME

// PSIO_REFLECT_ENUM(Enum, enumerator...) — at least one enumerator, each
// listed once, in any order (the order is the order of `names` / `values`).
#define PSIO_REFLECT_ENUM(ENUM, ...)                                                    \
   PSIO_REFLECT_ENUM_I_(ENUM, PSIO_PP_VARIADIC_TO_SEQ(__VA_ARGS__))
#define PSIO_REFLECT_ENUM_I_(ENUM, SEQ)                                                 \
   struct BOOST_PP_CAT(psio_enum_impl_, ENUM)                                           \
   {                                                                                     \
      using type = ENUM;                                                                 \
      static constexpr bool               is_reflected = true;                           \
      static constexpr ::std::string_view name         = BOOST_PP_STRINGIZE(ENUM);       \
      static constexpr ::std::size_t      count        = BOOST_PP_SEQ_SIZE(SEQ);         \
      static constexpr ::std::array<::std::string_view, count> names                     \
         = {BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_ENUM_NAME_, _, SEQ)};                   \
      static constexpr ::std::array<ENUM, count> values                                  \
         = {BOOST_PP_SEQ_FOR_EACH_I(PSIO_REFLECT_ENUM_VALUE_, ENUM, SEQ)};               \
      static constexpr const auto& _names_array_ = names;                                \
   };                                                                                    \
   static_assert(::std::is_enum_v<ENUM>, "psio: PSIO_REFLECT_ENUM takes an enum");      \
   static_assert(sizeof(::psio::detail::enum_bind<                                       \
                    ENUM, BOOST_PP_CAT(psio_enum_impl_, ENUM)>) > 0, "");
