#pragma once

// Structural-metadata macros: declarative WIT package + interface registration.
//
//   PSIO_PACKAGE(name, version)              — declares a WIT package
//   PSIO_INTERFACE(Tag, types(…), funcs(…))  — registers an interface
//   PSIO_USE(package, interface, version)    — cross-package use
//   PSIO_WORLD(name, imports(…), exports(…)) — top-level composition
//   PSIO_HOST_MODULE(Impl, interface(Tag, m…)…) — bind C++ host impl
//
// Each macro populates a compile-time, tag-keyed registry. wit_gen and
// wit_constexpr walk those registries to assemble WIT text or binary.
//
// Interface tags are user-authored types: the same struct/class that
// declares the interface's functions is used directly as the tag — no
// separate `*_interface_tag` helper.
//
// PSIO_INTERFACE(Tag, item...) takes these items, in any order, each optional:
//
//   types(T1, T2, ...)       the types the interface owns (records, resources)
//   funcs(func(f, p1, ...), g, ...)
//                            the functions, each with its parameter names. When
//                            funcs(...) is omitted the functions, their names,
//                            parameter names, pointers and the proxy come from
//                            reflect<Tag> — the methods listed by
//                            PSIO_REFLECT(Tag, method(f, p1, ...), ...).
//   name("psi:db/store")     overrides interface_info<Tag>::name (default: the
//                            tag's identifier). The name is both the WIT
//                            identifier and the runtime module name.
//   package(pkg)             the interface's package: a PSIO_PACKAGE(pkg, ...)
//                            visible from here.
//
// PSIO_PACKAGE and PSIO_INTERFACE may be written at global scope or inside any
// namespace, with an unqualified tag:
//
//   namespace chain::db {
//      PSIO_PACKAGE(chain_db, "1.0.0");
//      class store { ... };
//      PSIO_REFLECT(store, method(get, key), method(put, key, value))
//      PSIO_INTERFACE(store, name("psi:db/store"))
//   }
//
// The package of an interface is the first of:
//
//   0. the one its `package(pkg)` item names;
//   1. the PSIO_PACKAGE declared in the namespace PSIO_INTERFACE is written in,
//      or in the nearest enclosing namespace that declares one — when that
//      namespace declares exactly one. A package declared at global scope
//      applies only to interfaces registered at global scope;
//   2. the package named by PSIO_CURRENT_PACKAGE_ (the older pattern, for a
//      scope that declares several packages), under the same scope rule. The
//      macro stays defined for the rest of the translation unit and names its
//      package unqualified, so use it for packages declared at global scope
//      and `package(pkg)` inside a namespace:
//
//        PSIO_PACKAGE(wasi_clocks, "0.2.3");
//        #undef  PSIO_CURRENT_PACKAGE_
//        #define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_clocks)
//
//   3. a default package named after the tag's enclosing namespace
//      ("chain::db"; empty for a global tag) with an empty version.
//
// Internals: PSIO_PACKAGE generates `struct NAME_pkg_tag` in the current
// namespace; PSIO_INTERFACE generates `struct psio_interface_impl_Tag` there
// and binds it to the tag (and each listed type to the tag, for
// interface_of<T>) through a friend function defined by a class template
// instantiation — the one registration form that can be written inside a user
// namespace. interface_info<Tag> inherits the bound metadata.

#include <psio/reflect.hpp>  // brings in BOOST_PP and the existing PSIO_PP_* helpers

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/for_each_i.hpp>
#include <boost/preprocessor/seq/transform.hpp>
#include <boost/preprocessor/seq/size.hpp>
#include <boost/preprocessor/seq/to_tuple.hpp>
#include <boost/preprocessor/tuple/elem.hpp>
#include <boost/preprocessor/tuple/push_front.hpp>
#include <boost/preprocessor/tuple/rem.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>
#include <boost/preprocessor/variadic/size.hpp>
#include <boost/preprocessor/control/iif.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/facilities/check_empty.hpp>

// =====================================================================
// Preprocessor utilities specific to structural.hpp
//
// reflect.hpp already provides PSIO_PP_FIRST / PSIO_PP_APPLY_FIRST /
// PSIO_PP_MATCH. The helpers below cover what structural.hpp also
// needs: empty no-op, paren removal, seq→va_args, the funcs() entry
// grammar (`func(ident, args...)`), and boost-PP wrappers that tolerate
// empty sequences.
// =====================================================================

#define PSIO_PP_EMPTY(...)
#define PSIO_PP_REMOVE_PARENS(...) BOOST_PP_TUPLE_REM() __VA_ARGS__
#define PSIO_PP_SEQ_TO_VA_ARGS(seq) PSIO_PP_REMOVE_PARENS(BOOST_PP_SEQ_TO_TUPLE(seq))

// Seq variants that survive an empty sequence (BOOST_PP versions choke).
#define PSIO_PP_SEQ_TRANSFORM(op, data, seq) \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(seq), PSIO_PP_EMPTY, BOOST_PP_SEQ_TRANSFORM)(op, data, seq)

#define PSIO_PP_SEQ_FILTER(op, data, seq) \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(seq), PSIO_PP_EMPTY, BOOST_PP_SEQ_FILTER)(op, data, seq)

// =====================================================================
// funcs(...) entry grammar
//
// Each entry inside funcs(...) is either a bare identifier (e.g. `now`)
// or the canonical `func(ident, p1, p2, ...)` form (e.g.
// `func(subscribe_instant, when)`). Both normalize to the internal
// `_psio_method(ident, args...)` shape so the same downstream helpers
// apply uniformly.
// =====================================================================

#define PSIO_IFACE_MATCH_func(...) ~, 1
#define PSIO_IFACE_ARGS_func(...)  __VA_ARGS__

#define PSIO_IFACE_NORMALIZE_ENTRY(r, _, item)                               \
   BOOST_PP_IIF(                                                             \
       BOOST_PP_CHECK_EMPTY(item), ,                                         \
       BOOST_PP_IIF(PSIO_PP_MATCH(PSIO_IFACE_MATCH_, item),                  \
                    PSIO_IFACE_NORMALIZE_MATCHED,                            \
                    PSIO_IFACE_NORMALIZE_BARE)(item))

#define PSIO_IFACE_NORMALIZE_MATCHED(item) \
   _psio_method(BOOST_PP_CAT(PSIO_IFACE_ARGS_, item))
#define PSIO_IFACE_NORMALIZE_BARE(item) _psio_method(item)

// _psio_method(ident, args...) → ident
#define PSIO_IFACE_GET_IDENT(elem)         BOOST_PP_CAT(PSIO_IFACE_GET_IDENT_, elem)
#define PSIO_IFACE_GET_IDENT__psio_method(ident, ...) ident

// _psio_method(ident, args...) → args... (or nothing if no args)
#define PSIO_IFACE_GET_ARGS(elem)          BOOST_PP_CAT(PSIO_IFACE_GET_ARGS_, elem)
#define PSIO_IFACE_GET_ARGS__psio_method(ident, ...) __VA_ARGS__

// _psio_method(ident, a, b) → "a", "b"  (empty if no args)
#define PSIO_IFACE_QUOTED_ARGS(elem)                                              \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(PSIO_IFACE_GET_ARGS(elem)),                  \
                PSIO_PP_EMPTY,                                                    \
                PSIO_IFACE_QUOTED_ARGS_IMPL)(elem)
#define PSIO_IFACE_QUOTED_ARGS_IMPL(elem)                                         \
   PSIO_PP_SEQ_TO_VA_ARGS(BOOST_PP_SEQ_TRANSFORM(                                 \
       PSIO_IFACE_QUOTE_ONE, _, BOOST_PP_VARIADIC_TO_SEQ(PSIO_IFACE_GET_ARGS(elem))))
#define PSIO_IFACE_QUOTE_ONE(r, _, arg) BOOST_PP_STRINGIZE(arg)

// =====================================================================
// types(...) and funcs(...) wrappers — strip the keyword shell.
// =====================================================================

#define PSIO_IFACE_UNWRAP_TYPES(X)         BOOST_PP_CAT(PSIO_IFACE_UNWRAP_TYPES_, X)
#define PSIO_IFACE_UNWRAP_TYPES_types(...) __VA_ARGS__

#define PSIO_IFACE_UNWRAP_FUNCS(X)         BOOST_PP_CAT(PSIO_IFACE_UNWRAP_FUNCS_, X)
#define PSIO_IFACE_UNWRAP_FUNCS_funcs(...) __VA_ARGS__

// =====================================================================
// Per-entry expansion helpers
//
// Reflection only inspects the *type* of each interface method, not
// its address: decltype(&ANCHOR::m) is unevaluated and does not ODR-use
// the function, so the anchor struct may declare static methods with
// no inline bodies if desired (host-side bodies live wherever).
// =====================================================================

// Method addresses are evaluated only when a host registration needs them.
#define PSIO_IFACE_METHOD_(r, ANCHOR, I, elem) \
   if constexpr (Index == I) return &ANCHOR::PSIO_IFACE_GET_IDENT(elem);
#define PSIO_IFACE_PROXY_METHOD_(r, ANCHOR, I, elem) \
   template <typename... Args> \
   decltype(auto) PSIO_IFACE_GET_IDENT(elem)(Args&&... args) { \
      return static_cast<Dispatch&>(*this).template call<I, &ANCHOR::PSIO_IFACE_GET_IDENT(elem)>( \
         ::std::forward<Args>(args)...); \
   }

#define PSIO_IFACE_FN_TYPE(r, ANCHOR, elem) \
   decltype(&ANCHOR::PSIO_IFACE_GET_IDENT(elem))
#define PSIO_IFACE_FN_NAME(r, _, elem) \
   BOOST_PP_STRINGIZE(PSIO_IFACE_GET_IDENT(elem))
#define PSIO_IFACE_PARAM_NAMES_ENTRY(r, _, elem) \
   ::std::initializer_list<const char*>{PSIO_IFACE_QUOTED_ARGS(elem)},

// Reverse-lookup binding, emitted per type listed in `types(...)`.
#define PSIO_IFACE_REV_LOOKUP(s, TAG, T) \
   static_assert(sizeof(::psio::detail::interface_of_bind<T, TAG>) > 0, "");

namespace psio
{

namespace detail
{
   // The spelling of T as the compiler prints it ("a::b::C").
   template <typename T>
   constexpr std::string_view pretty_type_name() noexcept
   {
      const std::string_view p     = __PRETTY_FUNCTION__;
      const std::size_t      begin = p.find("T = ") + 4;
      std::size_t            end   = p.find_first_of(";]", begin);
      return p.substr(begin, end - begin);
   }
   // "a::b" of "a::b::C"; empty for a global name.
   constexpr std::string_view namespace_part(std::string_view name) noexcept
   {
      name                  = name.substr(0, name.find('<'));
      const std::size_t pos = name.rfind("::");
      return pos == std::string_view::npos ? std::string_view{} : name.substr(0, pos);
   }
   template <typename T>
   inline constexpr std::string_view namespace_of = namespace_part(pretty_type_name<T>());

   // Registration slots (see reflect.hpp's reflect_slot): a friend function
   // declared by the slot and defined by instantiating the binding.
   template <typename Tag>
   struct interface_slot
   {
      friend constexpr auto psio_interface_lookup(interface_slot);
   };
   template <typename Tag, typename Impl>
   struct interface_bind
   {
      friend constexpr auto psio_interface_lookup(interface_slot<Tag>) { return Impl{}; }
   };
   struct interface_unregistered
   {
   };
   template <typename Tag, typename = void>
   struct interface_base
   {
      using type = interface_unregistered;
   };
   template <typename Tag>
   struct interface_base<Tag, std::void_t<decltype(psio_interface_lookup(interface_slot<Tag>{}))>>
   {
      using type = decltype(psio_interface_lookup(interface_slot<Tag>{}));
   };

   template <typename T>
   struct interface_of_slot
   {
      friend constexpr auto psio_interface_of_lookup(interface_of_slot);
   };
   template <typename T, typename Tag>
   struct interface_of_bind
   {
      friend constexpr auto psio_interface_of_lookup(interface_of_slot<T>)
      {
         return std::type_identity<Tag>{};
      }
   };
   template <typename T, typename = void>
   struct interface_of_base
   {
   };
   template <typename T>
   struct interface_of_base<
      T, std::void_t<decltype(psio_interface_of_lookup(interface_of_slot<T>{}))>>
   {
      using type = typename decltype(psio_interface_of_lookup(interface_of_slot<T>{}))::type;
   };
}  // namespace detail

// ─── Reverse lookup: T → owning interface tag ────────────────────────
//
// `interface_of<T>::type` is the tag of the PSIO_INTERFACE that lists T in
// its types(…). There is no `type` member for interface-less types —
// schema-walking code uses that to detect "this is a raw reflected value,
// not an interface-owned one."
template <typename T>
struct interface_of : detail::interface_of_base<T>
{
};

// Reverse lookup: ImplClass → tuple of interface tags it implements.
// Specialized by PSIO_HOST_MODULE.  The Linker uses this in
// `provide(impl)` to find all slots that a host implementation fills.
template <typename ImplClass>
struct impl_of;

namespace detail
{

// ─── Package registry ─────────────────────────────────────────────────
//
// PSIO_PACKAGE(NAME, VERSION) generates `struct NAME##_pkg_tag` (carrying
// the name and version strings) in the current namespace.
// `package_info<NAME##_pkg_tag>` — what PSIO_PACKAGE_TYPE_(NAME) spells and
// what interface_info<Tag>::package is — exposes ::name and ::version.
template <typename Tag>
struct package_info : Tag
{
   using tag = Tag;
};

template <typename Tag>
struct package_marker
{
};

// The package of a tag with no PSIO_PACKAGE in scope: named after the tag's
// enclosing namespace, no version.
template <typename Tag>
struct namespace_package
{
   static constexpr std::string_view name    = namespace_of<Tag>;
   static constexpr std::string_view version = {};
};

// In-scope package lookup. PSIO_PACKAGE declares
// `NAME_pkg_tag psio_package_in_scope_(NAME_pkg_tag*)`; PSIO_INTERFACE calls
// the name unqualified with a probe convertible to any pointer, so ordinary
// lookup stops at the nearest enclosing namespace that declares a package,
// and the call resolves when that namespace declares exactly one.
struct package_probe
{
   template <typename T>
   operator T*() const;
};
struct no_package
{
};
no_package psio_package_in_scope_(...);

// A package declared in namespace `pkg` applies to an interface registered in
// namespace `scope` when it is the same namespace or, for a named namespace,
// an enclosing one.
constexpr bool package_applies(std::string_view pkg, std::string_view scope) noexcept
{
   return pkg == scope ||
          (!pkg.empty() && scope.size() > pkg.size() + 1 && scope.starts_with(pkg) &&
           scope.substr(pkg.size(), 2) == "::");
}

template <typename Probe, typename = void>
struct in_scope_package
{
   using type = no_package;
};
template <typename Probe>
struct in_scope_package<Probe, std::void_t<std::invoke_result_t<Probe, package_probe>>>
{
   using type = std::invoke_result_t<Probe, package_probe>;
};

template <typename Current>
struct current_package_tag
{
   using type = Current;  // a hand-written package type: its own scope
};
template <typename Tag>
struct current_package_tag<package_info<Tag>>
{
   using type = Tag;
};

// Current: PSIO_CURRENT_PACKAGE_ (void when never defined). Probe: the
// in-scope lookup. Scope: a type declared where PSIO_INTERFACE was written.
template <typename Current, typename Probe, typename Scope, typename Tag>
struct select_package
{
   using found = typename in_scope_package<Probe>::type;
   static constexpr int rule =
      !std::is_same_v<found, no_package> &&
            package_applies(namespace_of<found>, namespace_of<Scope>)
         ? 1
         : !std::is_void_v<Current> &&
                 package_applies(namespace_of<typename current_package_tag<Current>::type>,
                                 namespace_of<Scope>)
              ? 2
              : 3;
   using type = std::conditional_t<
      rule == 1, package_info<found>,
      std::conditional_t<rule == 2, Current, namespace_package<Tag>>>;
};

// ─── Interface registry ───────────────────────────────────────────────
//
// PSIO_INTERFACE(Tag, types(...), funcs(...)) binds the metadata that
// interface_info<Tag> inherits. The Tag is the user's interface class itself
// (the one carrying the functions for the operations).
//
// Exposed surface:
//   ::name        — string_view: the tag's identifier, or name("...")
//   ::package     — the interface's package (::name, ::version); see the
//                   lookup order at the top of this file
//   ::types       — std::tuple<T1, T2, ...> from types(...)
//   ::func_types  — std::tuple<decltype(&Tag::m1), ...>
//   ::func_names  — std::array<string_view, N>
//   ::param_names — initializer_list<const char*>[N]
//   ::method<I>() — &Tag::mI
//   ::proxy<Dispatch> — inherits Dispatch; each function is a member that
//                   calls Dispatch::call<I, &Tag::mI>(args...)
//
// An unregistered tag has none of these members.
template <typename Tag>
struct interface_info : interface_base<Tag>::type
{
};

template <typename Tag>
struct interface_marker
{
};

// The function list of an interface registered without funcs(...): taken
// from reflect<Tag>'s methods.
template <typename Tag>
struct reflected_interface
{
   static_assert(reflect<Tag>::is_reflected && reflect<Tag>::method_count > 0,
                 "psio: PSIO_INTERFACE without funcs(...) takes its functions from "
                 "PSIO_REFLECT(Tag, method(name, params...), ...), which must come first");
   template <std::size_t Index>
   static constexpr auto method()
   {
      return reflect<Tag>::template method_pointer<Index>;
   }
   template <typename Dispatch>
   using proxy = typename reflect<Tag>::template proxy<Dispatch>;
   template <typename Seq>
   struct func_types_of;
   template <std::size_t... Is>
   struct func_types_of<std::index_sequence<Is...>>
   {
      using type = std::tuple<typename reflect<Tag>::template method_type<Is>...>;
   };
   using func_types =
      typename func_types_of<std::make_index_sequence<reflect<Tag>::method_count>>::type;
   static constexpr auto  func_names  = reflect<Tag>::method_names;
   static constexpr auto& param_names = reflect<Tag>::method_param_name_lists;
};

// ─── Cross-package use registry — specialized by PSIO_USE ──────────
template <typename Tag>
struct use_info;

template <typename Tag>
struct use_marker
{
};

// ─── World registry — specialized by PSIO_WORLD ───────────────────
template <typename Tag>
struct world_info;

template <typename Tag>
struct world_marker
{
};

// ─── Per-(Impl, Tag) registry — one specialization per `interface(...)`
// entry inside PSIO_HOST_MODULE.  Carries:
//   ::host    = ImplClass
//   ::tag     = interface tag
//   ::methods = std::tuple<&Impl::m1, &Impl::m2, …>
//   ::names   = std::array<string_view, N>{"m1", "m2", …}
template <typename ImplClass, typename Tag>
struct iface_impl;

// Aggregate per-Impl registry — specialized by PSIO_HOST_MODULE.
// Carries the tuple of iface_impl<Impl, Tag> instantiations.
template <typename ImplClass>
struct impl_info;

// Variadic helper: PSIO_HOST_MODULE only emits a parenthesis-friendly
// list of tag types and the template machinery assembles
// `iface_impl<Impl, Tag>` tuples.  Avoids Boost.PP's struggle with
// commas inside `< >`.
template <typename Impl, typename... Tags>
using iface_impls_tuple = ::std::tuple<iface_impl<Impl, Tags>...>;

}  // namespace detail

// Convenience: T → package, transitively via interface_of.
template <typename T>
struct package_of
{
   using interface_tag        = typename interface_of<T>::type;
   using package              = typename detail::interface_info<interface_tag>::package;
   static constexpr std::string_view name = package::name;
};

}  // namespace psio

// =====================================================================
// PSIO_PACKAGE(NAME, VERSION)
// =====================================================================

// Sentinel — overwritten by a user `#define` (see the lookup order above).
#define PSIO_CURRENT_PACKAGE_ void

#define PSIO_PACKAGE_TYPE_(NAME) ::psio::detail::package_info<NAME##_pkg_tag>

// May be written at global scope or in any namespace; a trailing `;` is fine.
#define PSIO_PACKAGE(NAME, VERSION)                                          \
   struct NAME##_pkg_tag                                                     \
   {                                                                         \
      static constexpr ::std::string_view name    = #NAME;                   \
      static constexpr ::std::string_view version = VERSION;                 \
   };                                                                        \
   NAME##_pkg_tag psio_package_in_scope_(NAME##_pkg_tag*);                   \
   inline constexpr ::psio::detail::package_marker<NAME##_pkg_tag> _psio_pkg_##NAME{};

// =====================================================================
// PSIO_INTERFACE(Tag, [types(...)], [funcs(...)], [name("...")])
// =====================================================================

// Pick one kind of item out of the (up to four) items, whatever their order.
#define PSIO_IFACE_PICK_(WHAT, ...) PSIO_IFACE_PICK4_(WHAT, __VA_ARGS__, , , , )
#define PSIO_IFACE_PICK4_(WHAT, a, b, c, d, ...)                              \
   PSIO_IFACE_PICK1_(WHAT, a) PSIO_IFACE_PICK1_(WHAT, b)                      \
   PSIO_IFACE_PICK1_(WHAT, c) PSIO_IFACE_PICK1_(WHAT, d)
#define PSIO_IFACE_PICK1_(WHAT, item) PSIO_IFACE_PICK_##WHAT##_##item
#define PSIO_IFACE_PICK_T_
#define PSIO_IFACE_PICK_T_types(...) __VA_ARGS__
#define PSIO_IFACE_PICK_T_funcs(...)
#define PSIO_IFACE_PICK_T_package(...)
#define PSIO_IFACE_PICK_T_name(...)
#define PSIO_IFACE_PICK_F_
#define PSIO_IFACE_PICK_F_types(...)
#define PSIO_IFACE_PICK_F_funcs(...) __VA_ARGS__
#define PSIO_IFACE_PICK_F_package(...)
#define PSIO_IFACE_PICK_F_name(...)
#define PSIO_IFACE_PICK_H_
#define PSIO_IFACE_PICK_H_types(...)
#define PSIO_IFACE_PICK_H_funcs(...) 1
#define PSIO_IFACE_PICK_H_package(...)
#define PSIO_IFACE_PICK_H_name(...)
#define PSIO_IFACE_PICK_N_
#define PSIO_IFACE_PICK_N_types(...)
#define PSIO_IFACE_PICK_N_funcs(...)
#define PSIO_IFACE_PICK_N_package(...)
#define PSIO_IFACE_PICK_N_name(...) __VA_ARGS__
#define PSIO_IFACE_PICK_P_
#define PSIO_IFACE_PICK_P_types(...)
#define PSIO_IFACE_PICK_P_funcs(...)
#define PSIO_IFACE_PICK_P_name(...)
#define PSIO_IFACE_PICK_P_package(...) __VA_ARGS__

#define PSIO_INTERFACE(NAME, ...)                                                     \
   PSIO_INTERFACE_I_(NAME, (PSIO_IFACE_PICK_(T, __VA_ARGS__)),                    \
                     (PSIO_IFACE_PICK_(F, __VA_ARGS__)),                          \
                     PSIO_IFACE_PICK_(H, __VA_ARGS__),                            \
                     PSIO_IFACE_PICK_(N, __VA_ARGS__),                            \
                     PSIO_IFACE_PICK_(P, __VA_ARGS__))

#define PSIO_INTERFACE_I_(NAME, TYPES_P, FUNCS_P, HASFUNCS, NAMESTR, PKG)                  \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(HASFUNCS), PSIO_INTERFACE_REFLECTED_,            \
                PSIO_INTERFACE_LISTED_)(NAME, TYPES_P, FUNCS_P, NAMESTR, PKG)            \
   static_assert(sizeof(::psio::detail::interface_bind<                               \
                    NAME, BOOST_PP_CAT(psio_interface_impl_, NAME)>) > 0, "");        \
   inline constexpr ::psio::detail::interface_marker<NAME>                            \
       BOOST_PP_CAT(_psio_iface_, NAME){};                                            \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(PSIO_PP_REMOVE_PARENS(TYPES_P)), PSIO_PP_EMPTY,  \
                PSIO_INTERFACE_REV_)(NAME, TYPES_P)

#define PSIO_INTERFACE_REV_(NAME, TYPES_P) \
   BOOST_PP_SEQ_FOR_EACH(PSIO_IFACE_REV_LOOKUP, NAME, BOOST_PP_VARIADIC_TO_SEQ TYPES_P)

#define PSIO_IFACE_NAME_OR_(NAMESTR, NAME) \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(NAMESTR), BOOST_PP_STRINGIZE(NAME), NAMESTR)

#define PSIO_INTERFACE_PKG_NAMED_(NAME, PKG) \
      using package = ::psio::detail::package_info<BOOST_PP_CAT(PKG, _pkg_tag)>;
#define PSIO_INTERFACE_PKG_LOOKUP_(NAME, PKG)                                         \
      using package = typename ::psio::detail::select_package<                        \
         PSIO_CURRENT_PACKAGE_,                                                       \
         decltype([](auto psio_p_) -> decltype(psio_package_in_scope_(psio_p_)) {     \
            return psio_package_in_scope_(psio_p_);                                   \
         }),                                                                          \
         BOOST_PP_CAT(psio_interface_impl_, NAME), NAME>::type;

// name / package / types — common to both forms.
#define PSIO_INTERFACE_HEAD_(NAME, TYPES_P, NAMESTR, PKG)                                  \
      using _psio_tag_ = NAME;                                                        \
      static constexpr ::std::string_view name = PSIO_IFACE_NAME_OR_(NAMESTR, NAME);  \
      BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(PKG), PSIO_INTERFACE_PKG_LOOKUP_,             \
                   PSIO_INTERFACE_PKG_NAMED_)(NAME, PKG)                              \
      using types = ::std::tuple<PSIO_PP_REMOVE_PARENS(TYPES_P)>;

// No funcs(...): the function list is reflect<Tag>'s methods.
#define PSIO_INTERFACE_REFLECTED_(NAME, TYPES_P, FUNCS_P, NAMESTR, PKG)                   \
   struct BOOST_PP_CAT(psio_interface_impl_, NAME)                                    \
      : ::psio::detail::reflected_interface<NAME>                                     \
   {                                                                                  \
      PSIO_INTERFACE_HEAD_(NAME, TYPES_P, NAMESTR, PKG)                                  \
   };

#define PSIO_INTERFACE_LISTED_(NAME, TYPES_P, FUNCS_P, NAMESTR, PKG)                      \
   PSIO_INTERFACE_BODY(NAME, TYPES_P, NAMESTR, PKG,                                      \
                       BOOST_PP_SEQ_TRANSFORM(                                        \
                           PSIO_IFACE_NORMALIZE_ENTRY, _,                             \
                           PSIO_PP_VARIADIC_TO_SEQ FUNCS_P))

#define PSIO_INTERFACE_BODY(NAME, TYPES_P, NAMESTR, PKG, FUNCS_SEQ)                       \
   struct BOOST_PP_CAT(psio_interface_impl_, NAME)                                    \
   {                                                                                  \
      PSIO_INTERFACE_HEAD_(NAME, TYPES_P, NAMESTR, PKG)                                  \
      template <std::size_t Index> static constexpr auto method() { \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_IFACE_METHOD_, NAME, FUNCS_SEQ) \
      } \
      template <typename Dispatch> struct _psio_interface_proxy : Dispatch { \
         using Dispatch::Dispatch; \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO_IFACE_PROXY_METHOD_, _psio_tag_, FUNCS_SEQ) \
      }; \
      template <typename Dispatch> using proxy = _psio_interface_proxy<Dispatch>; \
      using func_types = ::std::tuple<PSIO_PP_SEQ_TO_VA_ARGS(                         \
          BOOST_PP_SEQ_TRANSFORM(PSIO_IFACE_FN_TYPE, NAME, FUNCS_SEQ))>;              \
      static constexpr ::std::array<::std::string_view,                               \
                                    BOOST_PP_SEQ_SIZE(FUNCS_SEQ)>                     \
          func_names{PSIO_PP_SEQ_TO_VA_ARGS(                                          \
              BOOST_PP_SEQ_TRANSFORM(PSIO_IFACE_FN_NAME, _, FUNCS_SEQ))};             \
      static constexpr ::std::initializer_list<const char*>                           \
          param_names[BOOST_PP_SEQ_SIZE(FUNCS_SEQ)] = {                               \
              BOOST_PP_SEQ_FOR_EACH(PSIO_IFACE_PARAM_NAMES_ENTRY, _, FUNCS_SEQ)};     \
   };

// =====================================================================
// PSIO_USE(PACKAGE, INTERFACE, VERSION) — cross-package interface use
// =====================================================================
//
// Records that the current scope wants to depend on a specific
// (PACKAGE, INTERFACE) at a given version.  Schema-walking and Linker
// code consumes this to compose worlds out of pre-existing interfaces.
//
// Example:
//
//   PSIO_USE(wasi_io, poll, "0.2.3")
//
// generates a unique tag struct and `use_info<Tag>` carrying the
// three string_views.  The tag's name is `<PACKAGE>_<INTERFACE>_use_tag`.

#define PSIO_USE_TAG_(PACKAGE, INTERFACE) \
   BOOST_PP_CAT(BOOST_PP_CAT(PACKAGE, _), BOOST_PP_CAT(INTERFACE, _use_tag))

#define PSIO_USE(PACKAGE, INTERFACE, VERSION)                                          \
   namespace psio::detail                                                              \
   {                                                                                   \
      struct PSIO_USE_TAG_(PACKAGE, INTERFACE)                                         \
      {                                                                                \
      };                                                                               \
      template <>                                                                      \
      struct use_info<PSIO_USE_TAG_(PACKAGE, INTERFACE)>                                \
      {                                                                                \
         static constexpr ::std::string_view package        = #PACKAGE;                \
         static constexpr ::std::string_view interface_name = #INTERFACE;              \
         static constexpr ::std::string_view version        = VERSION;                 \
      };                                                                               \
      inline constexpr use_marker<PSIO_USE_TAG_(PACKAGE, INTERFACE)>                    \
          BOOST_PP_CAT(BOOST_PP_CAT(_psio_use_, PACKAGE),                              \
                       BOOST_PP_CAT(_, INTERFACE)){};                                  \
   }

// =====================================================================
// PSIO_WORLD(NAME, imports(...), exports(...)) — top-level composition
// =====================================================================
//
// `imports(…)` lists use-tags (see PSIO_USE) and/or interface tags;
// `exports(…)` lists interface tags.  Both sides accept bare-name lists
// — no `func()` wrapping needed (worlds compose interfaces, not
// functions).
//
// The expansion lives in `namespace psio::detail`, so unqualified
// names are resolved via the chain  detail → psio → ::  — user
// classes at global scope are picked up through the global fallback.

#define PSIO_WORLD_UNWRAP_IMPORTS(X)         BOOST_PP_CAT(PSIO_WORLD_UNWRAP_IMPORTS_, X)
#define PSIO_WORLD_UNWRAP_IMPORTS_imports(...) __VA_ARGS__

#define PSIO_WORLD_UNWRAP_EXPORTS(X)         BOOST_PP_CAT(PSIO_WORLD_UNWRAP_EXPORTS_, X)
#define PSIO_WORLD_UNWRAP_EXPORTS_exports(...) __VA_ARGS__

#define PSIO_WORLD(NAME, IMPORTS_SPEC, EXPORTS_SPEC) \
   PSIO_WORLD_I_(NAME, PSIO_WORLD_UNWRAP_IMPORTS(IMPORTS_SPEC),  \
                       PSIO_WORLD_UNWRAP_EXPORTS(EXPORTS_SPEC))

#define PSIO_WORLD_I_(NAME, IMPORTS_VA, EXPORTS_VA)                                    \
   namespace psio::detail                                                              \
   {                                                                                   \
      struct BOOST_PP_CAT(NAME, _world_tag)                                            \
      {                                                                                \
      };                                                                               \
      template <>                                                                      \
      struct world_info<BOOST_PP_CAT(NAME, _world_tag)>                                \
      {                                                                                \
         static constexpr ::std::string_view name = #NAME;                             \
         using package = PSIO_CURRENT_PACKAGE_;                                        \
         using imports = ::std::tuple<IMPORTS_VA>;                                     \
         using exports = ::std::tuple<EXPORTS_VA>;                                     \
      };                                                                               \
      inline constexpr world_marker<BOOST_PP_CAT(NAME, _world_tag)>                    \
          BOOST_PP_CAT(_psio_world_, NAME){};                                          \
   }

// =====================================================================
// PSIO_HOST_MODULE(IMPL, interface(Tag, m1, m2, …)…) — bind C++ impl
// =====================================================================
//
// Declares ImplClass as the host implementation of one or more
// interfaces.  Each entry `interface(Tag, m1, m2, …)` lists the
// interface's tag and the member functions on Impl that fulfill it,
// in the same order as the interface's PSIO_INTERFACE funcs(…) entries.
//
// Effects:
//   • iface_impl<Impl, Tag> is specialized for each entry
//     (::tag / ::methods / ::names).
//   • impl_info<Impl> is specialized with ::interfaces =
//     iface_impls_tuple<Impl, Tag1, Tag2, …>.
//   • impl_of<Impl>::type = std::tuple<Tag1, Tag2, …> for Linker
//     reverse lookup.
//
// Must appear at namespace scope.  At least one entry is required.

#define PSIO_HOST_MODULE_ARGS_interface(...) __VA_ARGS__

#define PSIO_HOST_MODULE_HEAD_(x, ...) x
#define PSIO_HOST_MODULE_TAIL_(x, ...) __VA_ARGS__

#define PSIO_HOST_MODULE_TAG_(entry) \
   PSIO_HOST_MODULE_TAG_I_(BOOST_PP_CAT(PSIO_HOST_MODULE_ARGS_, entry))
#define PSIO_HOST_MODULE_TAG_I_(...) PSIO_HOST_MODULE_HEAD_(__VA_ARGS__)

#define PSIO_HOST_MODULE_METHODS_(entry) \
   PSIO_HOST_MODULE_METHODS_I_(BOOST_PP_CAT(PSIO_HOST_MODULE_ARGS_, entry))
#define PSIO_HOST_MODULE_METHODS_I_(...) PSIO_HOST_MODULE_TAIL_(__VA_ARGS__)

#define PSIO_HOST_MODULE_METHODS_SEQ_(entry) \
   BOOST_PP_VARIADIC_TO_SEQ(PSIO_HOST_MODULE_METHODS_(entry))

#define PSIO_HOST_MODULE_MEMBER_PTR_(s, IMPL, METHOD) &IMPL::METHOD
#define PSIO_HOST_MODULE_NAME_(s, _, METHOD)          BOOST_PP_STRINGIZE(METHOD)

#define PSIO_HOST_MODULE_EMIT_ENTRY_(r, IMPL, entry)                                   \
   template <>                                                                         \
   struct ::psio::detail::iface_impl<IMPL, ::PSIO_HOST_MODULE_TAG_(entry)>              \
   {                                                                                   \
      using host = IMPL;                                                               \
      using tag  = ::PSIO_HOST_MODULE_TAG_(entry);                                      \
      static constexpr auto methods = ::std::tuple{PSIO_PP_SEQ_TO_VA_ARGS(             \
         BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_MEMBER_PTR_, IMPL,                    \
                                PSIO_HOST_MODULE_METHODS_SEQ_(entry)))};               \
      static constexpr ::std::array<                                                   \
         ::std::string_view,                                                           \
         BOOST_PP_SEQ_SIZE(PSIO_HOST_MODULE_METHODS_SEQ_(entry))>                       \
         names{PSIO_PP_SEQ_TO_VA_ARGS(                                                 \
            BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_NAME_, _,                          \
                                   PSIO_HOST_MODULE_METHODS_SEQ_(entry)))};            \
   };

#define PSIO_HOST_MODULE_TAG_TYPE_(s, _, entry) ::PSIO_HOST_MODULE_TAG_(entry)

#define PSIO_HOST_MODULE(IMPL, ...)                                                    \
   BOOST_PP_SEQ_FOR_EACH(PSIO_HOST_MODULE_EMIT_ENTRY_, IMPL,                            \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))                        \
   template <>                                                                         \
   struct ::psio::impl_of<IMPL>                                                        \
   {                                                                                   \
      using type = ::std::tuple<PSIO_PP_SEQ_TO_VA_ARGS(                                \
         BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_TAG_TYPE_, _,                         \
                                BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))>;              \
   };                                                                                  \
   template <>                                                                         \
   struct ::psio::detail::impl_info<IMPL>                                              \
   {                                                                                   \
      using interfaces = ::psio::detail::iface_impls_tuple<IMPL,                        \
         PSIO_PP_SEQ_TO_VA_ARGS(                                                       \
            BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_TAG_TYPE_, _,                      \
                                   BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))>;           \
   };
