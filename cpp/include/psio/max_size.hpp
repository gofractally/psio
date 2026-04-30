#pragma once
//
// psio/max_size.hpp — compile-time upper bound on a type's encoded
// byte count, given its shape and its fields' length_bound annotations.
//
// `psio::max_encoded_size<T>()` returns `std::optional<std::size_t>`:
//   - some(N) when every variable-length sub-field of T carries a
//     `length_bound{.max=M}` (or exact=M) annotation, producing a
//     finite upper bound over all possible encoded values
//   - nullopt when any sub-field is unbounded — the encoder cannot
//     guarantee the output will fit in any predetermined width
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
// Consumers today: `auto_pssz_width_v<T>` in pssz.hpp picks the
// narrowest pssz width whose offsets can index every addressable byte
// in the worst case. Future consumers: buffer pre-sizing in any
// format, schema generation for fixed-width wire dialects.

#include <psio/annotate.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/shapes.hpp>
#include <psio/wrappers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

      // Header bookkeeping each pssz-style format imposes on a variable
      // field. We bill every variable field 4 bytes (offset or length
      // prefix under W=4). Pure fixed records pay zero.
      inline constexpr std::size_t kVariableFieldOverhead = 4;

      // Max encoded bytes for a bare type, with no field-annotation
      // context. Returns nullopt for unbounded variable-length types
      // (std::vector, std::string) — those must be used via a
      // length-bounded annotation or a bounded<> wrapper for the parent
      // to have a finite max.
      template <typename T>
      consteval std::optional<std::size_t> max_bare() noexcept;

      template <typename Annots>
      struct find_length_bound
      {
         static constexpr auto value =
            ::psio::find_spec<::psio::length_bound>(Annots{});
      };

      // Max encoded bytes for a *field* of FieldType, given its merged
      // effective-annotations tuple. Honors length_bound when the field
      // shape is variable-length (string, vector).
      template <typename FieldType, typename AnnotsTuple>
      consteval std::optional<std::size_t>
      max_field_with_annots(AnnotsTuple annots) noexcept
      {
         using F = std::remove_cvref_t<FieldType>;

         // String → length_bound is a byte count.
         if constexpr (std::is_same_v<F, std::string>)
         {
            auto lb = ::psio::find_spec<::psio::length_bound>(annots);
            if (lb)
               if (auto n = bound_max(*lb))
                  return static_cast<std::size_t>(*n) +
                         kVariableFieldOverhead;
            return std::nullopt;
         }
         // Vector → length_bound is an element count; multiply by
         // per-element max. Fails if the element type is itself
         // unbounded (e.g. vector<string> without nested bounds).
         else if constexpr (is_std_vector<F>::value)
         {
            using E = typename F::value_type;
            auto lb = ::psio::find_spec<::psio::length_bound>(annots);
            if (!lb)
               return std::nullopt;
            auto n = bound_max(*lb);
            if (!n)
               return std::nullopt;
            auto e = max_bare<E>();
            if (!e)
               return std::nullopt;
            return static_cast<std::size_t>(*n) * *e +
                   kVariableFieldOverhead;
         }
         // Optional → transparent w.r.t. annotations.  The bound
         // applies to the inner U, not the optional itself.  Wire
         // overhead is conservatively billed as: U's max payload
         // (computed via the same field machinery, so a length_bound
         // on the optional<U> field reaches U) + 1-byte union tag
         // (always added — it over-counts by 1 byte for fixed-U
         // cases where pssz uses span equality instead of a selector,
         // which is acceptable for an upper bound) + the optional's
         // own offset slot (kVariableFieldOverhead).
         else if constexpr (is_std_optional<F>::value)
         {
            using U = typename F::value_type;
            // Recurse via max_field_with_annots so length_bound
            // propagates to U.  If U is itself a string/vector, the
            // recursion will add U's own kVariableFieldOverhead — for
            // an outer optional<string> with W=4 this conservatively
            // bills two slots (the outer and the inner), one of which
            // is fictitious in pssz but not in frac/bin.  Slack is
            // acceptable for an upper bound.
            auto inner = max_field_with_annots<U>(annots);
            if (!inner)
               return std::nullopt;
            return *inner + 1u + kVariableFieldOverhead;
         }
         else
         {
            // Fixed shapes, records, primitives — bounds don't apply.
            return max_bare<F>();
         }
      }

      template <typename T>
      consteval std::optional<std::size_t> max_bare() noexcept
      {
         using U = std::remove_cvref_t<T>;

         if constexpr (std::is_same_v<U, bool>)
            return 1;
         else if constexpr (std::is_same_v<U, ::psio::uint256>)
            return 32;
         else if constexpr (std::is_same_v<U, ::psio::uint128> ||
                            std::is_same_v<U, ::psio::int128>)
            return 16;
         else if constexpr (std::is_enum_v<U>)
            return sizeof(std::underlying_type_t<U>);
         else if constexpr (std::is_arithmetic_v<U>)
            return sizeof(U);
         else if constexpr (is_std_array<U>::value)
         {
            using E = typename is_std_array<U>::element_type;
            auto e  = max_bare<E>();
            if (!e)
               return std::nullopt;
            return is_std_array<U>::n * *e;
         }
         else if constexpr (::psio::Record<U>)
         {
            using R                 = ::psio::reflect<U>;
            constexpr std::size_t N = R::member_count;
            std::size_t total = 0;
            bool        bounded = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto add_field = [&]<std::size_t I>() {
                  using FT = typename R::template member_type<I>;
                  using eff =
                     ::psio::effective_annotations_for<
                        U, FT, R::template member_pointer<I>>;
                  auto m = max_field_with_annots<FT>(eff::value);
                  if (!m)
                  {
                     bounded = false;
                     return;
                  }
                  total += *m;
               };
               (add_field.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            if (!bounded)
               return std::nullopt;
            return total;
         }
         else
         {
            // std::string, std::vector<_> — require annotation context.
            return std::nullopt;
         }
      }

   }  // namespace detail::max_size_impl

   // Public entry point — returns the upper bound in bytes over all
   // possible encoded values of T, or nullopt when any sub-field is
   // unbounded. The bound assumes pssz W=4 offset overhead and is a
   // correct upper bound for every narrower pssz width.
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
   // Format consumers (pssz/frac/psch/pjson) use this for offset-width
   // selection: pick the narrowest width whose addressable range fits.
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
