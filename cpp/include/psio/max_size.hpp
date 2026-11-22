#pragma once
//
// psio/max_size.hpp — compile-time upper bound on a type's encoded
// byte count, given its shape and its fields' length_bound annotations.
//
// `psio::max_encoded_size<T>()` returns `std::optional<std::size_t>`:
//   - some(N) when annotations and declared child budgets provide a finite
//     conservative upper bound, including container bookkeeping
//   - nullopt when any sub-field is unbounded or bound arithmetic overflows
//
// Field-level bounds participate through psio's standard annotation
// merge: effective_annotations_for<Rec, F, MemberPtr> gathers
// member > wrapper > type specs, and the walk below reads length_bound
// out of that merged tuple. Member beats wrapper beats type, so a field
// can tighten a type-level default.
//
// Assumed wire overhead is pssz-family with W=4 (the widest offset
// size). That is the conservative choice — if max fits in 0xff with
// W=4 offsets, it certainly fits with W=1 offsets; the narrower the
// offset, the fewer overhead bytes spent, so the max only shrinks.
//
// `auto_pssz_width_v<T>` uses this bound only when T has no explicit
// maxDynamicData budget. The W=4 bookkeeping intentionally permits threshold
// slack rather than making width selection depend on its own result.

#include <psio/adapter.hpp>
#include <psio/annotate.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/detail/assoc.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/shapes.hpp>
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <limits>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio {

   namespace detail::max_size_impl {

      // ── Shape classifiers local to this walk ──────────────────────────────

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector<std::vector<E, A>> : std::true_type {};

      template <typename T>
      struct is_std_array : std::false_type {};
      template <typename E, std::size_t N>
      struct is_std_array<std::array<E, N>> : std::true_type
      {
         static constexpr std::size_t n = N;
         using element_type             = E;
      };

      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename U>
      struct is_std_optional<std::optional<U>> : std::true_type
      {
         using inner_type = U;
      };

      // utf8_string<N> carries its byte bound in the type; utf8_string<> is
      // unbounded, exactly like std::string.
      template <typename T>
      struct is_utf8_string : std::false_type {};
      template <std::size_t N>
      struct is_utf8_string<::psio::utf8_string<N>> : std::true_type
      {
         static constexpr std::size_t limit = N;
      };

      // Extract the effective count cap from a length_bound: exact wins,
      // then max. min alone does not bound the upper side.
      constexpr std::optional<std::uint32_t>
      bound_max(const length_bound& lb) noexcept
      {
         if (lb.exact)
            return lb.exact;
         if (lb.max)
            return lb.max;
         return std::nullopt;
      }

      // These are conservative PSSZ payload bounds with W=4 bookkeeping.
      // An enclosing record adds its own field slot separately. Declared child
      // budgets are reserved in full so compatible child growth cannot silently
      // change an inferred parent width.
      using bound = std::optional<std::size_t>;
      constexpr bound add(bound a, bound b) noexcept
      {
         if (!a || !b || *b > std::numeric_limits<std::size_t>::max() - *a)
            return std::nullopt;
         return *a + *b;
      }
      constexpr bound multiply(std::size_t count, bound value) noexcept
      {
         if (count == 0) return 0;
         if (!value || *value > std::numeric_limits<std::size_t>::max() / count)
            return std::nullopt;
         return count * *value;
      }
      template <typename T>
      consteval bound max_bare() noexcept;

      // Fixed payloads occupy their bytes inline. Adapter payloads and unknown
      // shapes are conservatively treated as variable for bound bookkeeping.
      template <typename T>
      consteval bool fixed_payload() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (::psio::has_adapter_v<U, ::psio::binary_category>)
            return false;
         else if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U> ||
                            ::psio::TimeValue<U> ||
                            std::is_same_v<U, ::psio::uint128> ||
                            std::is_same_v<U, ::psio::int128> ||
                            std::is_same_v<U, ::psio::uint256>)
            return true;
         else if constexpr (is_std_array<U>::value)
            return fixed_payload<typename is_std_array<U>::element_type>();
         else if constexpr (::psio::is_transparent_v<U>)
            return fixed_payload<::psio::transparent_inner_t<U>>();
         else if constexpr (::psio::Record<U>)
         {
            if constexpr (!::psio::is_dwnc_v<U>) return false;
            else
            {
               using R = ::psio::reflect<U>;
               return []<std::size_t... Is>(std::index_sequence<Is...>) {
                  return ((fixed_payload<typename R::template member_type<Is>>() &&
                           !::psio::has_as_override_v<typename ::psio::effective_annotations_for<
                              U, typename R::template member_type<Is>,
                              R::template member_pointer<Is>>::value_t>) && ...);
               }(std::make_index_sequence<R::member_count>{});
            }
         }
         else return false;
      }

      template <typename T>
      consteval bound child_payload() noexcept
      {
         constexpr auto budget = ::psio::max_dynamic_data_v<std::remove_cvref_t<T>>;
         if constexpr (budget.has_value()) return budget;
         else return max_bare<T>();
      }

      template <typename FieldType, typename AnnotsTuple>
      consteval bound max_field_with_annots(AnnotsTuple annots) noexcept
      {
         using F = std::remove_cvref_t<FieldType>;
         if constexpr (::psio::has_as_override_v<AnnotsTuple>)
            return std::nullopt; // opaque alternate presentation
         else if constexpr (::psio::max_dynamic_data_v<F>.has_value())
            return ::psio::max_dynamic_data_v<F>;
         else if constexpr (std::is_same_v<F, std::string>)
         {
            auto lb = ::psio::find_spec<::psio::length_bound>(annots);
            if (!lb) return std::nullopt;
            auto count = bound_max(*lb);
            return count ? bound{*count} : bound{};
         }
         else if constexpr (::psio::detail::is_nested_v<F>)
         {
            // A nested value has no bound of its own; a member length_bound
            // bounds its byte count, as it does a byte list's.
            auto lb = ::psio::find_spec<::psio::length_bound>(annots);
            if (!lb) return std::nullopt;
            auto count = bound_max(*lb);
            return count ? bound{*count} : bound{};
         }
         else if constexpr (is_std_vector<F>::value)
         {
            using E = typename F::value_type;
            auto lb = ::psio::find_spec<::psio::length_bound>(annots);
            if (!lb) return std::nullopt;
            auto count = bound_max(*lb);
            if (!count) return std::nullopt;
            auto element = child_payload<E>();
            if constexpr (!fixed_payload<E>()) element = add(element, 4);
            return multiply(*count, element);
         }
         else if constexpr (::psio::detail::is_assoc_v<F>)
            // A map / set is the list of its entries / elements.
            return max_field_with_annots<::psio::detail::assoc_list_t<F>>(annots);
         else if constexpr (is_std_optional<F>::value)
         {
            using E = typename F::value_type;
            // One selector is conservative for fixed nonempty payloads, whose
            // existing optional representation needs no selector.
            return add(max_field_with_annots<E>(annots), 1);
         }
         else return child_payload<F>();
      }

      template <typename T>
      consteval bound max_bare() noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (::psio::has_adapter_v<U, ::psio::binary_category>)
            return std::nullopt;
         else if constexpr (std::is_same_v<U, bool>) return 1;
         else if constexpr (std::is_same_v<U, ::psio::uint256>) return 32;
         else if constexpr (std::is_same_v<U, ::psio::uint128> ||
                            std::is_same_v<U, ::psio::int128>) return 16;
         else if constexpr (std::is_enum_v<U>) return sizeof(std::underlying_type_t<U>);
         else if constexpr (::psio::TimeValue<U>) return sizeof(::psio::time_rep_t<U>);
         else if constexpr (std::is_arithmetic_v<U>) return sizeof(U);
         else if constexpr (is_utf8_string<U>::value)
         {
            // A variable payload of at most N bytes, wherever it appears
            // (bare, member, element, optional payload). No member-level
            // length_bound is needed; the unbounded form has no bound.
            if constexpr (U::is_bounded) return is_utf8_string<U>::limit;
            else return std::nullopt;
         }
         else if constexpr (is_std_array<U>::value)
         {
            using E = typename is_std_array<U>::element_type;
            auto element = child_payload<E>();
            if constexpr (!fixed_payload<E>()) element = add(element, 4);
            return multiply(is_std_array<U>::n, element);
         }
         else if constexpr (::psio::is_transparent_v<U>)
            // The member's bound, its declared budget included.
            return child_payload<::psio::transparent_inner_t<U>>();
         else if constexpr (::psio::Record<U>)
         {
            using R = ::psio::reflect<U>;
            bound total = ::psio::is_dwnc_v<U> ? 0 : 4;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto field = [&]<std::size_t I>() {
                  using F = typename R::template member_type<I>;
                  using eff = ::psio::effective_annotations_for<
                     U, F, R::template member_pointer<I>>;
                  auto payload = max_field_with_annots<F>(eff::value);
                  if constexpr (!fixed_payload<F>() ||
                                ::psio::has_as_override_v<typename eff::value_t>)
                     payload = add(payload, 4);
                  total = add(total, payload);
               };
               (field.template operator()<Is>(), ...);
            }(std::make_index_sequence<R::member_count>{});
            return total;
         }
         else if constexpr (std::is_same_v<U, std::string> || ::psio::detail::is_assoc_v<U> ||
                            is_std_vector<U>::value || is_std_optional<U>::value)
            return max_field_with_annots<U>(::psio::effective_annotations<U, ::psio::type<U>{}>::value);
         else return std::nullopt;
      }

   }  // namespace detail::max_size_impl

   // Public entry point — returns the upper bound in bytes over all
   // possible encoded values of T, or nullopt when any sub-field is
   // unbounded. The bound assumes pssz W=4 offset overhead and is a
   // conservative upper bound for every narrower pssz width. Child records with
   // declared budgets reserve their full budget, not only their present fields.
   template <typename T>
   consteval std::optional<std::size_t> max_encoded_size() noexcept
   {
      return detail::max_size_impl::max_bare<T>();
   }

   // ── effective_max_dynamic_v ───────────────────────────────────────────
   //
   // Composes an explicit `maxDynamicData(N)` cap (from annotate.hpp's
   // `max_dynamic_data_v<T>`) with the bound inferred from per-field
   // `length_bound` annotations (`max_encoded_size<T>`).  The explicit
   // cap WINS DOWNWARD only — overriding upward defeats the purpose of
   // an upper-bound assertion.
   //
   //   cap present, bound present → min(cap, bound)
   //   cap present, bound nullopt → cap          (cap is the only bound)
   //   cap nullopt, bound present → bound        (no override; trust inferred)
   //   cap nullopt, bound nullopt → nullopt      (caller picks conservative width)
   //
   // Retained downward-bound utility for consumers needing the tighter actual
   // bound. Automatic PSSZ width selection uses the declared budget directly
   // instead: its lifetime width promise must not shrink to current fields.
   template <typename T>
   inline constexpr std::optional<std::size_t> effective_max_dynamic_v = []
   {
      constexpr auto cap   = max_dynamic_data_v<T>;
      constexpr auto bound = max_encoded_size<T>();
      if constexpr (cap.has_value() && bound.has_value())
         return std::optional<std::size_t>{
            (*cap < *bound) ? *cap : *bound};
      else if constexpr (cap.has_value())
         return cap;
      else
         return bound;
   }();

}  // namespace psio
