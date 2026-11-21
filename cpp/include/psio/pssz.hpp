#pragma once
//
// psio/pssz.hpp — pSSZ (PsiSSZ) format tag.
//
// Successor to the legacy fracpack prototype, which is retained for benchmarks.
//
// pSSZ is a variable-width variant of SSZ — offsets and container
// headers use 1, 2, or 4 bytes rather than always 4. The width is
// picked automatically per type by `auto_pssz_width_v<T>` from
// `max_encoded_size<T>()` (see max_size.hpp):
//
//   max ≤ 0xff   → W = 1
//   max ≤ 0xffff → W = 2
//   else         → W = 4
//
// Users shrink the width by attaching `length_bound{.max = N}` to
// variable-length fields (member annotation) or types (type annotation
// / wrapper). Unbounded types fall back to W = 4.
//
// Only one public tag: `psio::pssz`. The width choice is a compile-
// time detail of the encode/decode path, not a user-facing knob.
//
// `psio::pssz_<W>` exists as an explicit-width tag for
// byte-parity regression tests against v1; production code should
// always use `pssz` and let width selection happen automatically.
//
// Scope (Phase 7 MVP) matches Phase 6: primitives, std::array,
// std::vector, std::string, std::optional, reflected records.
// Out of scope (follow-ups): the "skip_header on DWNC" optimization,
// runtime size assertions against `max_encoded_size<T>()`.

#include <psio/cpo.hpp>
#include <psio/detail/unaligned_iter.hpp>
#include <psio/detail/validate_depth.hpp>
#include <psio/detail/pssz_record_layout.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/max_size.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/validate_strict_walker.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <variant>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio::schema_types
{
   // Fwd decl: Box<T> is the unique_ptr-backed cycle-breaker that lives
   // in schema_ir.hpp; declared here so pssz can recognise it as a
   // transparent wrapper without taking a circular dependency on the
   // schema header.
   template <typename T>
   struct Box;
}  // namespace psio::schema_types

namespace psio {

   struct pssz;  // fwd — used by adapter-dispatch trait below.
   template <std::size_t W>
   struct pssz_;  // explicit-width tag (byte-parity testing).

   namespace detail::pssz_impl {

      // ── maxDynamicData enforcement ────────────────────────────────────────
      //
      // Throws codec_exception when the encoded total exceeds T's
      // `maxDynamicData(N)` cap.  The schema author asserted the cap;
      // an encoder that produces > N bytes has violated the wire-size
      // contract, and silently widening defeats the predictability the
      // cap was added to provide.
      //
      // Note: cap is enforced against TOTAL encoded size (fixed +
      // dynamic regions).  The spec describes it as a dynamic-region
      // cap, but the conservative ceiling on total is correct (any
      // total > cap implies dynamic > cap - fixed_overhead, and the
      // fixed overhead for pssz is small).  Refine to a strictly
      // dynamic-only check if the fixed-region delta becomes material.
      template <typename T>
      inline void enforce_max_dynamic(std::size_t total)
      {
         constexpr auto cap = ::psio::max_dynamic_data_v<T>;
         if constexpr (cap.has_value())
         {
            if (total > *cap)
               ::psio::codec_abort(codec_error{
                  "pssz: encoded size exceeds maxDynamicData cap",
                  static_cast<std::uint32_t>(total), "pssz"});
         }
      }

      // noexcept counterpart for `validate<T>(...)` — returns
      // codec_status::error on cap excess instead of throwing, matching
      // the contract of psio::validate.  Returns ok() when no cap is
      // declared on T.
      template <typename T>
      inline codec_status check_max_dynamic(
         std::span<const char> bytes) noexcept
      {
         constexpr auto cap = ::psio::max_dynamic_data_v<T>;
         if constexpr (cap.has_value())
         {
            if (bytes.size() > *cap)
               return codec_status{codec_error{
                  "pssz: encoded size exceeds maxDynamicData cap",
                  static_cast<std::uint32_t>(bytes.size()), "pssz"}};
         }
         return codec_status{};
      }

      // ── Width → offset integer type ───────────────────────────────────────
      template <std::size_t W>
      struct width_info;

      template <>
      struct width_info<1>
      {
         using offset_t = std::uint8_t;
      };
      template <>
      struct width_info<2>
      {
         using offset_t = std::uint16_t;
      };
      template <>
      struct width_info<4>
      {
         using offset_t = std::uint32_t;
      };

      // ── Box<T> detector ───────────────────────────────────────────────
      //
      // psio::schema_types::Box<T> is the unique_ptr-backed wrapper that
      // breaks recursive cycles in the Schema IR.  pssz treats Box<T>
      // as fully transparent — every CPO forwards to T, wire bytes are
      // identical to what `T` alone produces.  Defined up here so
      // is_fixed<Box<T>> and fixed_size_of<Box<T>> below see it.
      template <typename T>
      struct is_psio_box : std::false_type
      {
      };
      template <typename U>
      struct is_psio_box<::psio::schema_types::Box<U>> : std::true_type
      {
         using elem = U;
      };
      template <typename T>
      inline constexpr bool is_psio_box_v = is_psio_box<T>::value;

      // ── Shape classification (same as SSZ) ────────────────────────────────

      template <typename T>
      struct is_fixed : std::false_type
      {
      };
      template <>
      struct is_fixed<bool> : std::true_type
      {
      };
      template <typename T>
         requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
      struct is_fixed<T> : std::true_type
      {
      };
      template <typename T, std::size_t N>
      struct is_fixed<std::array<T, N>> : is_fixed<T>
      {
      };

      template <std::size_t N>
      struct is_fixed<::psio::bitvector<N>> : std::true_type
      {
      };

      // 128 / 256-bit integers — fixed-width raw LE bytes.
      template <>
      struct is_fixed<::psio::uint128> : std::true_type
      {
      };
      template <>
      struct is_fixed<::psio::int128> : std::true_type
      {
      };
      template <>
      struct is_fixed<::psio::uint256> : std::true_type
      {
      };

      // Box<T> is transparent for fixed-ness: a Box<fixed-T> is fixed,
      // a Box<variable-T> is variable.  The forward declaration above
      // is enough for is_fixed specialisation; the inner type's
      // fixed-ness drives the result.
      template <typename T>
      struct is_fixed<::psio::schema_types::Box<T>> : is_fixed<T>
      {
      };

      template <typename T>
      struct is_std_tuple : std::false_type {};
      template <typename... Ts>
      struct is_std_tuple<std::tuple<Ts...>> : std::true_type {};
      template <typename T>
      inline constexpr bool is_std_tuple_v = is_std_tuple<T>::value;

      template <typename T>
      struct record_traits : ::psio::reflect<T> {};
      template <typename... Ts>
      struct record_traits<std::tuple<Ts...>>
      {
         static constexpr std::size_t member_count = sizeof...(Ts);
         template <std::size_t I>
         using member_type = std::tuple_element_t<I, std::tuple<Ts...>>;
      };

      template <typename T, std::size_t I, bool = is_std_tuple_v<T>>
      struct record_member_annotations
      {
         using R = record_traits<T>;
         using F = typename R::template member_type<I>;
         using value_t = typename ::psio::effective_annotations_for<
            T, F, R::template member_pointer<I>>::value_t;
      };
      template <typename T, std::size_t I>
      struct record_member_annotations<T, I, true>
      {
         using value_t = std::tuple<>;
      };

      template <std::size_t I, typename T>
      decltype(auto) record_member(T&& value) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (is_std_tuple_v<U>)
            return std::get<I>(std::forward<T>(value));
         else
            return std::forward<T>(value).*(record_traits<U>::template member_pointer<I>);
      }

      template <typename T>
      concept Record = ::psio::Reflected<T> || is_std_tuple_v<T>;

      template <typename T>
      inline constexpr bool is_fixed_v = is_fixed<T>::value;

      template <Record T, std::size_t... Is>
      consteval bool record_fields_all_fixed(std::index_sequence<Is...>)
      {
         using R = record_traits<T>;
         return ((is_fixed_v<typename R::template member_type<Is>> &&
                  !::psio::has_as_override_v<typename record_member_annotations<T, Is>::value_t>) && ...);
      }

      template <Record T>
      consteval bool record_all_fixed()
      {
         using R = record_traits<T>;
         return record_fields_all_fixed<T>(
            std::make_index_sequence<R::member_count>{});
      }

      template <typename T>
      inline constexpr bool has_binary_adapter_v =
         ::psio::has_adapter_v<T, ::psio::binary_category>;

      // A Record is pssz-fixed iff it is DWNC AND all fields are fixed.
      // DWNC skips the u{W} header, so a DWNC all-fixed record is
      // truly constant-size on the wire. Non-DWNC records carry the
      // header and (in a future trailing-pruning implementation)
      // variable-size fixed regions, so they go through the record
      // encode path rather than being inlined as fixed.
      template <Record T>
         requires(!has_binary_adapter_v<T>)
      struct is_fixed<T>
         : std::bool_constant<::psio::is_dwnc_v<T> && record_all_fixed<T>()>
      {
      };

      template <typename T>
         requires(has_binary_adapter_v<T>)
      struct is_fixed<T> : std::false_type
      {
      };

      // ── fixed_size_of ─────────────────────────────────────────────────────
      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept;

      template <Record T, std::size_t... Is>
      consteval std::size_t sum_record_fields(std::index_sequence<Is...>)
      {
         using R = record_traits<T>;
         return (fixed_size_of<typename R::template member_type<Is>>() + ... + 0);
      }

      // Size contribution of member I to the pSSZ record's fixed_region.
      // Variable members occupy a `W`-byte offset slot (W = 1, 2, or 4
      // bytes per the per-record width selector).  See ssz.hpp for the
      // rationale on why these are free function templates rather than
      // lambdas-in-folds.
      template <std::size_t W, Record T, std::size_t I>
      consteval std::size_t pssz_member_fixed_size_simple()
      {
         using R = record_traits<T>;
         using F = typename R::template member_type<I>;
         if constexpr (is_fixed_v<F>) return fixed_size_of<F>();
         else                          return W;
      }

      template <std::size_t W, Record T, std::size_t I>
      consteval std::size_t pssz_member_fixed_size_full()
      {
         using R = record_traits<T>;
         using F = typename R::template member_type<I>;
         using eff = typename record_member_annotations<T, I>::value_t;
         constexpr bool override_v = ::psio::has_as_override_v<eff>;
         if constexpr (!override_v && is_fixed_v<F>)
            return fixed_size_of<F>();
         else
            return W;
      }

      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept
      {
         if constexpr (is_psio_box_v<T>)
            return fixed_size_of<typename is_psio_box<T>::elem>();
         else if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            return 16;
         else if constexpr (std::is_arithmetic_v<T>)
            return sizeof(T);
         else if constexpr (requires { T::bytes_value; T::size_value; })
         {
            // psio::bitvector<N> — declares both size_value and
            // bytes_value; the latter is (N+7)/8.
            return T::bytes_value;
         }
         else if constexpr (requires { typename T::value_type; } &&
                            requires { std::tuple_size<T>::value; })
         {
            using E = typename T::value_type;
            return std::tuple_size<T>::value * fixed_size_of<E>();
         }
         else if constexpr (Record<T>)
         {
            using R = record_traits<T>;
            return sum_record_fields<T>(
               std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return 0;
         }
      }

      // ── std-container trait helpers ───────────────────────────────────────
      template <typename T>
      struct is_std_array : std::false_type
      {
      };
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_vector : std::false_type
      {
      };
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_optional : std::false_type
      {
      };
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type
      {
      };

      using ::psio::detail::is_std_variant;

      template <typename T>
      inline constexpr bool is_std_array_v    = is_std_array<T>::value;
      template <typename T>
      inline constexpr bool is_std_vector_v   = is_std_vector<T>::value;
      template <typename T>
      inline constexpr bool is_std_optional_v = is_std_optional<T>::value;
      template <typename T>
      inline constexpr bool is_std_variant_v  = is_std_variant<T>::value;

      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio::bitvector<N>> : std::true_type {};

      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio::bitlist<N>> : std::true_type {};

      template <typename T>
      inline constexpr bool is_bitvector_v = is_bitvector<T>::value;
      template <typename T>
      inline constexpr bool is_bitlist_v = is_bitlist<T>::value;

      // Whether an encoded value has a positive minimum length. This is
      // deliberately independent of fixed-size classification: extensible
      // records always have a header, while a zero-sized DWNC unit does not.
      template <std::size_t W, typename T>
      consteval bool positive_min_size()
      {
         if constexpr (is_psio_box_v<T>)
            return positive_min_size<W, typename is_psio_box<T>::elem>();
         else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::pssz, T>)
            return false; // An adapter without a minimum-size contract may encode empty.
         else if constexpr (is_std_optional_v<T>)
            return !positive_min_size<W, typename T::value_type>();
         else if constexpr (is_std_variant_v<T> || is_bitlist_v<T>)
            return true;
         else if constexpr (is_fixed_v<T>)
            return fixed_size_of<T>() != 0;
         else if constexpr (is_std_array_v<T>)
            return std::tuple_size_v<T> != 0;
         else if constexpr (Record<T>)
            return !::psio::is_dwnc_v<T> || record_traits<T>::member_count != 0;
         else
            return false;
      }

      template <Record T, std::size_t I>
      inline constexpr bool omittable_field_v =
         is_std_optional_v<typename record_traits<T>::template member_type<I>> &&
         !::psio::has_as_override_v<typename record_member_annotations<T, I>::value_t>;

      template <Record T>
      std::size_t emitted_field_count(const T& value) noexcept
      {
         using R = record_traits<T>;
         if constexpr (::psio::is_dwnc_v<T>)
            return R::member_count;
         else
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               std::size_t count = 0;
               (([&] {
                  if constexpr (omittable_field_v<T, Is>)
                  {
                     if (record_member<Is>(value).has_value()) count = Is + 1;
                  }
                  else count = Is + 1;
               }()), ...);
               return count;
            }(std::make_index_sequence<R::member_count>{});
      }

      template <std::size_t W, Record T, std::size_t... Is>
      consteval auto record_layout(std::index_sequence<Is...>)
      {
         return std::array<::psio::detail::pssz_field_layout, sizeof...(Is)>{
            ::psio::detail::pssz_field_layout{
               pssz_member_fixed_size_full<W, T, Is>(),
               ::psio::has_as_override_v<typename record_member_annotations<T, Is>::value_t> ||
                  !is_fixed_v<typename record_traits<T>::template member_type<Is>>,
               omittable_field_v<T, Is>}...};
      }

      template <std::size_t W, typename T>
      codec_status check_encode_depth(const T& value, std::size_t depth = 0)
      {
         if (depth > kMaxValidationDepth)
            return codec_fail("pssz: max depth exceeded", 0, "pssz");
         if constexpr (is_psio_box_v<T>)
         {
            if (!value.value) return codec_fail("pssz: null Box", 0, "pssz");
            return check_encode_depth<W>(*value.value, depth);
         }
         else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::pssz, T>)
            return codec_ok();
         else if constexpr (is_std_optional_v<T>)
         {
            if (value) return check_encode_depth<W>(*value, depth + 1);
         }
         else if constexpr (is_std_array_v<T> || is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_std_vector_v<T> && is_fixed_v<E>)
               if constexpr (fixed_size_of<E>() == 0)
                  if (!value.empty()) return codec_fail("pssz: nonempty zero-stride list", 0, "pssz");
            for (const auto& element : value)
            {
               auto status = check_encode_depth<W, E>(element, depth + 1);
               if (!status.ok()) return status;
            }
         }
         else if constexpr (is_std_variant_v<T>)
            return std::visit([&](const auto& element) {
               return check_encode_depth<W>(element, depth + 1);
            }, value);
         else if constexpr (Record<T>)
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               codec_status status;
               const auto count = emitted_field_count(value);
               (([&] {
                  if (status.ok() && Is < count)
                     status = check_encode_depth<W>(record_member<Is>(value), depth + 1);
               }()), ...);
               return status;
            }(std::make_index_sequence<record_traits<T>::member_count>{});
         else if constexpr (std::is_same_v<T, std::string>)
            return ::psio::utf8_spec::validate({value.data(), value.size()});
         return codec_ok();
      }

      // ── Sizing ────────────────────────────────────────────────────────────
      //
      // For pSSZ, offsets cost W bytes. The shape remains the same as SSZ.

      template <std::size_t W, typename T>
      std::size_t size_of_v(const T& v) noexcept
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            return Proj::packsize(v);
         }
         else if constexpr (is_psio_box_v<T>)
            return size_of_v<W>(*v.value);
         else if constexpr (is_fixed_v<T>)
            return fixed_size_of<T>();
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            std::size_t           total = N * W;
            for (const auto& x : v)
               total += size_of_v<W>(x);
            return total;
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
               return v.size() * fixed_size_of<E>();
            else
            {
               std::size_t total = v.size() * W;
               for (const auto& x : v)
                  total += size_of_v<W>(x);
               return total;
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
            return v.size();
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            // Positive-minimum V: length disambiguates None from Some.
            // Zero-minimum V: one selector byte distinguishes empty Some.
            if constexpr (positive_min_size<W, V>())
               return v.has_value() ? size_of_v<W>(*v) : 0;
            else
               return 1 + (v.has_value() ? size_of_v<W>(*v) : 0);
         }
         else if constexpr (is_bitlist_v<T>)
            return (v.size() + 8) / 8;
         else if constexpr (is_std_variant_v<T>)
         {
            std::size_t total = 1;
            std::visit([&](const auto& alt) { total += size_of_v<W>(alt); },
                       v);
            return total;
         }
         else if constexpr (Record<T>)
         {
            using R = record_traits<T>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) noexcept {
               std::size_t total = 0;
               const auto count = emitted_field_count(v);
               // u{W} fixed_size header — DWNC types skip it.
               if constexpr (!::psio::is_dwnc_v<T>)
                  total += W;
               (
                  ([&]
                   {
                      if (Is >= count) return;
                      using F = typename R::template member_type<Is>;
                      const auto& fref = record_member<Is>(v);
                      using eff =
                         typename record_member_annotations<T, Is>::value_t;
                      constexpr bool override_v =
                         ::psio::has_as_override_v<eff>;
                      if constexpr (override_v)
                      {
                         // Member-level `as<Tag>` forces the field through
                         // the named adapter — emit a W-byte offset slot
                         // plus the adapter's payload.
                         using Tag = ::psio::adapter_tag_of_t<eff>;
                         using Proj = ::psio::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         total += W + Proj::packsize(fref);
                      }
                      else if constexpr (is_fixed_v<F>)
                         total += fixed_size_of<F>();
                      else
                         total += W + size_of_v<W>(fref);
                   }()),
                  ...);
               return total;
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::pssz: unsupported type in size_of_v");
            return 0;
         }
      }

      // ── Encoding ──────────────────────────────────────────────────────────

      using sink_t = std::vector<char>;

      template <std::size_t W, typename Sink>
      void write_offset(Sink& s, std::size_t pos, std::size_t value)
      {
         using O = typename width_info<W>::offset_t;
         if (value > std::numeric_limits<O>::max())
            ::psio::codec_abort(codec_error{"pssz: offset exceeds selected width", 0, "pssz"});
         O ov   = static_cast<O>(value);
         s.rewrite(pos, &ov, W);
      }

      template <typename Sink>
      void skip_bytes(Sink& sink, std::size_t size)
      {
         constexpr auto chunk = std::numeric_limits<std::int32_t>::max();
         while (size > static_cast<std::size_t>(chunk))
         {
            sink.skip(chunk);
            size -= chunk;
         }
         sink.skip(static_cast<std::int32_t>(size));
      }

      // Sink-templated encoder. Top-level tag_invoke pre-sizes the
      // output buffer via size_of_v then walks once with
      // fast_buf_stream — same algorithm v1's convert_to_pssz uses.
      template <std::size_t W, typename T, typename Sink>
      void encode_value(const T& v, Sink& s)
      {
         // Adapter dispatch must be the FIRST branch of the if-else
         // chain — separating it from the chain leaves the trailing
         // `else { static_assert(sizeof(T)==0, ...) }` instantiated for
         // non-reflected adapter types, which trips at compile time
         // even though the runtime path returns from the adapter
         // branch. Chaining via `else if constexpr` discards the tail.
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            // Adapter encode methods assume a vector<char>-like sink;
            // route through a temp vector for non-vector sinks.
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            sink_t tmp;
            Proj::encode(v, tmp);
            s.write(tmp.data(), tmp.size());
         }
         else if constexpr (is_psio_box_v<T>)
         {
            encode_value<W>(*v.value, s);
            return;
         }
         else if constexpr (std::is_same_v<T, bool>)
            s.write(v ? '\x01' : '\x00');
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            s.write(v.limb, 32);
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            s.write(&v, 16);
         else if constexpr (std::is_arithmetic_v<T>)
            s.put(v);
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
            {
               for (const auto& x : v)
                  encode_value<W>(x, s);
            }
            else
            {
               const std::size_t table = s.written();
               skip_bytes(s, N * W);
               for (std::size_t i = 0; i < N; ++i)
               {
                  write_offset<W>(s, table + i * W,
                                  s.written() - table);
                  encode_value<W>(v[i], s);
               }
            }
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               // Bulk memcpy for arithmetic OR memcpy-layout records.
               constexpr bool is_arith =
                  std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
               constexpr bool is_memcpy_record =
                  Record<E> && std::is_trivially_copyable_v<E> &&
                  fixed_size_of<E>() == sizeof(E);
               if constexpr (is_arith || is_memcpy_record)
               {
                  if (!v.empty())
                     s.write(v.data(), v.size() * sizeof(E));
               }
               else
               {
                  for (const auto& x : v)
                     encode_value<W>(x, s);
               }
            }
            else
            {
               const std::size_t n     = v.size();
               const std::size_t table = s.written();
               skip_bytes(s, n * W);
               for (std::size_t i = 0; i < n; ++i)
               {
                  write_offset<W>(s, table + i * W,
                                  s.written() - table);
                  encode_value<W>(v[i], s);
               }
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
            s.write(v.data(), v.size());
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            // pSSZ optional selector rule (matches v1):
            //   min_encoded_size(V) > 0 → no selector;
            //     the byte-span length (0 vs fixed_size) disambiguates.
            //   min_encoded_size(V) == 0  → 1-byte Union
            //     selector (0x00 = None, 0x01 + payload = Some).
            if constexpr (positive_min_size<W, V>())
            {
               if (v.has_value())
                  encode_value<W>(*v, s);
               // None → no bytes appended.
            }
            else
            {
               if (v.has_value())
               {
                  s.write('\x01');
                  encode_value<W>(*v, s);
               }
               else
               {
                  s.write('\x00');
               }
            }
         }
         else if constexpr (is_bitvector_v<T>)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(v.data(), nbytes);
         }
         else if constexpr (is_bitlist_v<T>)
         {
            const std::size_t bit_count   = v.size();
            const std::size_t total_bytes = (bit_count + 8) / 8;
            constexpr std::size_t kStack  = 256;
            char              stack_buf[kStack];
            std::vector<char> heap_buf;
            char*             p;
            if (total_bytes <= kStack)
            {
               p = stack_buf;
               std::memset(p, 0, total_bytes);
            }
            else
            {
               heap_buf.assign(total_bytes, 0);
               p = heap_buf.data();
            }
            auto bytes = v.bytes();
            if (!bytes.empty())
               std::memcpy(p, bytes.data(),
                           std::min(bytes.size(), total_bytes));
            p[bit_count >> 3] |=
               static_cast<char>(1u << (bit_count & 7u));
            s.write(p, total_bytes);
         }
         else if constexpr (is_std_variant_v<T>)
         {
            // pSSZ Union: 1-byte selector + value (same shape as SSZ;
            // offsets widths only affect containers holding variable
            // fields, not the selector byte itself).
            static_assert(std::variant_size_v<T> <= 256,
                          "pssz variant selector is u8 (≤ 256 alternatives)");
            s.write(static_cast<char>(v.index()));
            std::visit(
               [&](const auto& alt) { encode_value<W>(alt, s); }, v);
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path for DWNC packed memcpy-layout records:
            // wire bytes match memory bytes exactly, no u{W} header.
            // (For non-DWNC records the u{W} header forces the
            // walker; for DWNC + padded records the walker is also
            // needed because layout doesn't match.)
            if constexpr (::psio::is_dwnc_v<T> &&
                          std::is_trivially_copyable_v<T> &&
                          is_fixed_v<T>)
               if constexpr (fixed_size_of<T>() == sizeof(T))
            {
               s.write(&v, sizeof(T));
               return;
            }
            using R = record_traits<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               const auto count = emitted_field_count(v);
               std::size_t fixed_region = 0;
               ((Is < count ? fixed_region += pssz_member_fixed_size_full<W, T, Is>() : 0), ...);

               // u{W} fixed_size header — fracpack's forward-compat
               // slot. DWNC types skip it (matches v1 pssz: the header
               // width is header_bytes == W). Non-DWNC records always
               // carry the header so a newer decoder with more fields
               // knows how many bytes of fixed region were actually
               // written.
               if constexpr (!::psio::is_dwnc_v<T>)
               {
                  using O = typename width_info<W>::offset_t;
                  if (fixed_region > std::numeric_limits<O>::max())
                     ::psio::codec_abort(codec_error{"pssz: record header exceeds selected width", 0, "pssz"});
                  O hdr   = static_cast<O>(fixed_region);
                  s.write(&hdr, W);
               }

               const std::size_t container_start = s.written();
               skip_bytes(s, fixed_region);
               std::size_t fixed_cursor = container_start;

               (
                  ([&]
                   {
                      if (Is >= count) return;
                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         record_member<Is>(v);
                      using eff =
                         typename record_member_annotations<T, Is>::value_t;
                      constexpr bool override_v =
                         ::psio::has_as_override_v<eff>;

                      if constexpr (!override_v && is_fixed_v<F>)
                      {
                         if constexpr (std::is_same_v<F, bool>)
                         {
                            char b = fref ? '\x01' : '\x00';
                            s.rewrite(fixed_cursor, &b, 1);
                         }
                         else if constexpr (std::is_arithmetic_v<F>)
                         {
                            s.rewrite(fixed_cursor, &fref, sizeof(F));
                         }
                         else if constexpr (is_std_array_v<F>)
                         {
                            using E = typename F::value_type;
                            if constexpr (std::is_arithmetic_v<E> &&
                                          !std::is_same_v<E, bool>)
                            {
                               s.rewrite(fixed_cursor, fref.data(),
                                         fixed_size_of<F>());
                            }
                            else
                            {
                               sink_t tmp;
                               ::psio::vector_stream vs{tmp};
                               encode_value<W>(fref, vs);
                               s.rewrite(fixed_cursor, tmp.data(),
                                         tmp.size());
                            }
                         }
                         else
                         {
                            sink_t tmp;
                            ::psio::vector_stream vs{tmp};
                            encode_value<W>(fref, vs);
                            s.rewrite(fixed_cursor, tmp.data(),
                                      tmp.size());
                         }
                         fixed_cursor += fixed_size_of<F>();
                      }
                      else
                      {
                         write_offset<W>(s, fixed_cursor,
                                         s.written() - container_start);
                         fixed_cursor += W;
                         if constexpr (override_v)
                         {
                            using Tag = ::psio::adapter_tag_of_t<eff>;
                            using Proj = ::psio::adapter<
                               std::remove_cvref_t<F>, Tag>;
                            sink_t tmp;
                            Proj::encode(fref, tmp);
                            s.write(tmp.data(), tmp.size());
                         }
                         else
                         {
                            encode_value<W>(fref, s);
                         }
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::pssz: unsupported type in encode_value");
         }
      }

      // ── Decoding ──────────────────────────────────────────────────────────

      template <std::size_t W>
      std::uint32_t read_offset(std::span<const char> src, std::size_t pos)
      {
         using O = typename width_info<W>::offset_t;
         O v{};
         std::memcpy(&v, src.data() + pos, W);
         return static_cast<std::uint32_t>(v);
      }

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end);

      template <std::size_t W, Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src, std::size_t pos,
                              std::size_t end, T& out,
                              std::index_sequence<Is...>);

      // In-place decode for std::string and bulk-memcpy std::vector.
      // Avoids the temp + move-assign overhead for these field types
      // when called from the Record walker. Falls back to
      // decode_value for other types.
      //
      // Adapter dispatch must come first — see frac_impl::decode_into for
      // why. The record walker has already sliced [pos, end) to exactly
      // the adapter's payload, so we hand it straight to the adapter.
      template <std::size_t W, typename T>
      void decode_into(std::span<const char> src, std::size_t pos,
                       std::size_t end, T& out)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            out = Proj::decode(
               std::span<const char>(src.data() + pos, end - pos));
            return;
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            // pSSZ string: raw bytes from pos to end (no length).
            out.assign(src.data() + pos, src.data() + end);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && std::is_trivially_copyable_v<E> &&
               is_fixed_v<E> && fixed_size_of<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               // Alignment-safe — see detail/unaligned_iter.hpp.
               const std::size_t n = (end - pos) / sizeof(E);
               psio::detail::assign_from_wire(out, src.data() + pos, n);
            }
            else
            {
               out = decode_value<W, T>(src, pos, end);
            }
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path mirrors decode_value's, but writes
            // directly into `out` instead of returning a fresh T.
            if constexpr (::psio::is_dwnc_v<T> &&
                          std::is_trivially_copyable_v<T> &&
                          is_fixed_v<T>)
               if constexpr (fixed_size_of<T>() == sizeof(T))
            {
               std::memcpy(&out, src.data() + pos, sizeof(T));
               return;
            }
            using R = record_traits<T>;
            record_decode_into<W, T>(
               src, pos, end, out,
               std::make_index_sequence<R::member_count>{});
         }
         else
         {
            out = decode_value<W, T>(src, pos, end);
         }
      }

      template <std::size_t W, typename T>
         requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
      T decode_arith(std::span<const char> src, std::size_t pos)
      {
         T out{};
         std::memcpy(&out, src.data() + pos, sizeof(T));
         return out;
      }

      template <std::size_t W, typename T, std::size_t N>
      std::array<T, N> decode_array(std::span<const char> src, std::size_t pos,
                                    std::size_t end)
      {
         std::array<T, N> out{};
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            for (std::size_t i = 0; i < N; ++i)
               out[i] = decode_value<W, T>(src, pos + i * esz,
                                           pos + (i + 1) * esz);
         }
         else if constexpr (N > 0)
         {
            std::array<std::uint32_t, N> offsets{};
            for (std::size_t i = 0; i < N; ++i)
               offsets[i] = read_offset<W>(src, pos + i * W);
            for (std::size_t i = 0; i < N; ++i)
            {
               const std::size_t beg = pos + offsets[i];
               const std::size_t fin =
                  (i + 1 < N) ? (pos + offsets[i + 1]) : end;
               out[i] = decode_value<W, T>(src, beg, fin);
            }
         }
         return out;
      }

      template <std::size_t W, typename T>
      std::vector<T> decode_vector(std::span<const char> src, std::size_t pos,
                                   std::size_t end)
      {
         std::vector<T> out;
         if (pos >= end)
            return out;
         if constexpr (is_fixed_v<T>)
         {
            constexpr std::size_t esz = fixed_size_of<T>();
            if constexpr (esz == 0) return out;
            const std::size_t n   = (end - pos) / esz;
            // Bulk-memcpy fast path covers arithmetic primitives AND
            // memcpy-layout DWNC packed records (sizeof matches wire).
            constexpr bool is_arith =
               std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;
            constexpr bool is_memcpy_record =
               Record<T> && std::is_trivially_copyable_v<T> &&
               fixed_size_of<T>() == sizeof(T);
            if constexpr (is_arith || is_memcpy_record)
            {
               // Alignment-safe — see detail/unaligned_iter.hpp.
               psio::detail::assign_from_wire(out, src.data() + pos, n);
            }
            else if constexpr (Record<T>)
            {
               // In-place decode — single bulk zero-init via resize, then
               // write directly into each slot. Mirrors the ssz path.
               out.resize(n);
               using R = record_traits<T>;
               for (std::size_t i = 0; i < n; ++i)
                  record_decode_into<W, T>(
                     src, pos + i * esz, pos + (i + 1) * esz, out[i],
                     std::make_index_sequence<R::member_count>{});
            }
            else
            {
               out.reserve(n);
               for (std::size_t i = 0; i < n; ++i)
                  out.push_back(decode_value<W, T>(src, pos + i * esz,
                                                    pos + (i + 1) * esz));
            }
         }
         else
         {
            if (end - pos < W)
               return out;
            const std::uint32_t first = read_offset<W>(src, pos);
            const std::size_t   n     = first / W;
            std::vector<std::uint32_t> offsets(n);
            for (std::size_t i = 0; i < n; ++i)
               offsets[i] = read_offset<W>(src, pos + i * W);
            if constexpr (Record<T>)
            {
               // In-place: resize then walk each element directly into
               // its slot, avoiding push_back's move-construct.
               out.resize(n);
               using R = record_traits<T>;
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::size_t beg = pos + offsets[i];
                  const std::size_t fin =
                     (i + 1 < n) ? (pos + offsets[i + 1]) : end;
                  record_decode_into<W, T>(
                     src, beg, fin, out[i],
                     std::make_index_sequence<R::member_count>{});
               }
            }
            else
            {
               out.reserve(n);
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::size_t beg = pos + offsets[i];
                  const std::size_t fin =
                     (i + 1 < n) ? (pos + offsets[i + 1]) : end;
                  out.push_back(decode_value<W, T>(src, beg, fin));
               }
            }
         }
         return out;
      }

      // Forward declaration — used by decode_vector to decode record
      // elements in-place. Definition appears later in this file.
      template <std::size_t W, Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src, std::size_t pos,
                              std::size_t end, T& out,
                              std::index_sequence<Is...> seq)
      {
         using R = record_traits<T>;
         constexpr auto layout = record_layout<W, T>(seq);
         std::array<::psio::detail::pssz_field_span, R::member_count> spans{};
         bool unknown = false;
         auto status = ::psio::detail::pssz_record_spans<W>(
            src, pos, end, ::psio::is_dwnc_v<T>, layout, spans, unknown);
         if (!status.ok()) ::psio::codec_abort(status.error());
         (([&] {
            using F = typename R::template member_type<Is>;
            auto& field = record_member<Is>(out);
            if (!spans[Is].present)
            {
               if constexpr (omittable_field_v<T, Is>) field.reset();
               return;
            }
            using eff = typename record_member_annotations<T, Is>::value_t;
            if constexpr (::psio::has_as_override_v<eff>)
            {
               using Proj = ::psio::adapter<std::remove_cvref_t<F>, ::psio::adapter_tag_of_t<eff>>;
               field = Proj::decode(src.subspan(spans[Is].begin, spans[Is].end - spans[Is].begin));
            }
            else decode_into<W, F>(src, spans[Is].begin, spans[Is].end, field);
         }()), ...);
      }

      template <std::size_t W, Record T, std::size_t... Is>
      T record_decode(std::span<const char> src, std::size_t pos,
                      std::size_t end, std::index_sequence<Is...> seq)
      {
         T out{};
         record_decode_into<W, T>(src, pos, end, out, seq);
         return out;
      }

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end)
      {
         // Adapter dispatch must chain into the if-else below so the
         // trailing `else { static_assert }` is discarded for non-
         // reflected adapter types — see encode_value's note.
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            return Proj::decode(
               std::span<const char>(src.data() + pos, end - pos));
         }
         else if constexpr (is_psio_box_v<T>)
         {
            using E = typename is_psio_box<T>::elem;
            return T{decode_value<W, E>(src, pos, end)};
         }
         else if constexpr (std::is_same_v<T, bool>)
            return static_cast<unsigned char>(src[pos]) != 0;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            T out{};
            std::memcpy(out.limb, src.data() + pos, 32);
            return out;
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, 16);
            return out;
         }
         else if constexpr (std::is_arithmetic_v<T>)
            return decode_arith<W, T>(src, pos);
         else if constexpr (std::is_same_v<T, std::string>)
            return std::string(src.data() + pos, src.data() + end);
         else if constexpr (is_std_array_v<T>)
            return decode_array<W, typename T::value_type,
                                std::tuple_size<T>::value>(src, pos, end);
         else if constexpr (is_std_vector_v<T>)
            return decode_vector<W, typename T::value_type>(src, pos, end);
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            if constexpr (positive_min_size<W, V>())
            {
               // No selector: empty means None; nonempty means Some.
               if (pos >= end)
                  return std::optional<V>{};
               return std::optional<V>{decode_value<W, V>(src, pos, end)};
            }
            else
            {
               // 1-byte Union selector at pos: 0x00 = None, 0x01 = Some.
               if (pos >= end)
                  return std::optional<V>{};
               const std::uint8_t sel =
                  static_cast<unsigned char>(src[pos]);
               if (sel == 0)
                  return std::optional<V>{};
               return std::optional<V>{
                  decode_value<W, V>(src, pos + 1, end)};
            }
         }
         else if constexpr (is_std_variant_v<T>)
         {
            const auto idx = static_cast<std::size_t>(
               static_cast<unsigned char>(src[pos]));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  W,
                                  std::variant_alternative_t<Is, T>>(
                                  src, pos + 1, end)},
                       true)
                    : false) ||
                  ...);
               (void)found;
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return out;
         }
         else if constexpr (is_bitvector_v<T>)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            T                     out{};
            if constexpr (nbytes > 0)
               std::memcpy(out.data(), src.data() + pos, nbytes);
            return out;
         }
         else if constexpr (is_bitlist_v<T>)
         {
            T out;
            if (end <= pos)
               return out;
            const std::size_t total_bytes = end - pos;
            const unsigned char last =
               static_cast<unsigned char>(src[end - 1]);
            if (last == 0)
               return out;
            int delim_bit = 7;
            while (delim_bit >= 0 && !((last >> delim_bit) & 1))
               --delim_bit;
            const std::size_t bit_count =
               (total_bytes - 1) * 8 +
               static_cast<std::size_t>(delim_bit);
            auto& bits = out.storage();
            bits.resize(bit_count);
            for (std::size_t i = 0; i < bit_count; ++i)
            {
               const unsigned char b =
                  static_cast<unsigned char>(src[pos + (i >> 3)]);
               bits[i] = (b >> (i & 7)) & 1;
            }
            return out;
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path for DWNC packed memcpy-layout records
            // (where wire bytes match memory bytes). Single-record
            // decode hits this; vec<E> already had its own bulk path.
            if constexpr (::psio::is_dwnc_v<T> &&
                          std::is_trivially_copyable_v<T> &&
                          is_fixed_v<T>)
               if constexpr (fixed_size_of<T>() == sizeof(T))
            {
               T out;
               std::memcpy(&out, src.data() + pos, sizeof(T));
               return out;
            }
            using R = record_traits<T>;
            return record_decode<W, T>(
               src, pos, end, std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::pssz: unsupported type in decode_value");
         }
      }

      // ── Structural validation ─────────────────────────────────────────────
      //
      // Full byte-level walker. Mirrors the SSZ shape but with W-byte
      // offsets and the u{W} fixed_region header on non-DWNC records.

      template <std::size_t W, typename T>
      codec_status validate_value(std::span<const char> src, std::size_t pos,
                                  std::size_t end,
                                  std::size_t depth = 0, bool* has_unknown = nullptr) noexcept;

      template <std::size_t W, typename E>
      codec_status validate_vector_payload(std::span<const char> src,
                                           std::size_t            pos,
                                           std::size_t            end,
                                           std::size_t            depth, bool* has_unknown) noexcept
      {
         if (pos > end || end > src.size())
            return codec_fail("pssz: vector span out of bounds",
                              static_cast<std::uint32_t>(pos), "pssz");
         if (pos == end)
            return codec_ok();

         if constexpr (is_fixed_v<E>)
         {
            constexpr std::size_t esz = fixed_size_of<E>();
            if constexpr (esz == 0)
               return codec_fail("pssz: nonempty zero-stride list", 0, "pssz");
            else
            {
            if ((end - pos) % esz != 0)
               return codec_fail("pssz: vector span not a multiple of "
                                 "element size",
                                 static_cast<std::uint32_t>(pos), "pssz");
            const std::size_t n = (end - pos) / esz;
            for (std::size_t i = 0; i < n; ++i)
            {
               auto st = validate_value<W, E>(src, pos + i * esz, pos + (i + 1) * esz,
                                               depth + 1, has_unknown);
               if (!st.ok()) return st;
            }
            return codec_ok();
            }
         }
         else
         {
            // Variable-element list with W-byte offset table.
            if (end - pos < W)
               return codec_fail("pssz: variable list missing offset table",
                                 static_cast<std::uint32_t>(pos), "pssz");
            const std::size_t span = end - pos;
            const std::uint32_t first = read_offset<W>(src, pos);
            if (first == 0 || first % W != 0 || first > span)
               return codec_fail("pssz: invalid first list offset",
                                 static_cast<std::uint32_t>(pos), "pssz");
            const std::size_t n = first / W;
            if (span < n * W)
               return codec_fail("pssz: list offset table truncated",
                                 static_cast<std::uint32_t>(pos), "pssz");
            std::uint32_t prev = first;
            for (std::size_t i = 1; i < n; ++i)
            {
               std::uint32_t off_i = read_offset<W>(src, pos + i * W);
               if (off_i < prev || off_i > span)
                  return codec_fail("pssz: list offset out of range",
                                    static_cast<std::uint32_t>(pos + i * W),
                                    "pssz");
               prev = off_i;
            }
            for (std::size_t i = 0; i < n; ++i)
            {
               std::uint32_t off_i = read_offset<W>(src, pos + i * W);
               std::size_t stop  = (i + 1 < n)
                                        ? read_offset<W>(src, pos + (i + 1) * W)
                                        : span;
               auto st = validate_value<W, E>(src, pos + off_i, pos + stop,
                                               depth + 1, has_unknown);
               if (!st.ok()) return st;
            }
            return codec_ok();
         }
      }

      template <std::size_t W, typename E, std::size_t N>
      codec_status validate_array_payload(std::span<const char> src,
                                          std::size_t            pos,
                                          std::size_t            end,
                                          std::size_t            depth, bool* has_unknown) noexcept
      {
         if constexpr (is_fixed_v<E>)
         {
            constexpr std::size_t esz = fixed_size_of<E>();
            if (end - pos != N * esz)
               return codec_fail("pssz: array buffer too small",
                                 static_cast<std::uint32_t>(pos), "pssz");
            for (std::size_t i = 0; i < N; ++i)
            {
               auto st = validate_value<W, E>(src, pos + i * esz, pos + (i + 1) * esz,
                                               depth + 1, has_unknown);
               if (!st.ok()) return st;
            }
            return codec_ok();
         }
         else
         {
            if constexpr (N == 0)
               return pos == end ? codec_ok() : codec_fail("pssz: empty array has trailing bytes", 0, "pssz");
            if (end - pos < N * W)
               return codec_fail("pssz: array offset table truncated",
                                 static_cast<std::uint32_t>(pos), "pssz");
            const std::size_t span = end - pos;
            const std::uint32_t first = read_offset<W>(src, pos);
            if (first != N * W || first > span)
               return codec_fail("pssz: invalid array first offset",
                                 static_cast<std::uint32_t>(pos), "pssz");
            std::uint32_t prev = first;
            for (std::size_t i = 1; i < N; ++i)
            {
               std::uint32_t off_i = read_offset<W>(src, pos + i * W);
               if (off_i < prev || off_i > span)
                  return codec_fail("pssz: array offset out of range",
                                    static_cast<std::uint32_t>(pos + i * W),
                                    "pssz");
               prev = off_i;
            }
            for (std::size_t i = 0; i < N; ++i)
            {
               std::uint32_t off_i = read_offset<W>(src, pos + i * W);
               std::size_t stop  = (i + 1 < N)
                                        ? read_offset<W>(src, pos + (i + 1) * W)
                                        : span;
               auto st = validate_value<W, E>(src, pos + off_i, pos + stop,
                                               depth + 1, has_unknown);
               if (!st.ok()) return st;
            }
            return codec_ok();
         }
      }

      template <std::size_t W, std::size_t MaxN>
      codec_status validate_bitlist_payload(std::span<const char> src,
                                            std::size_t            pos,
                                            std::size_t            end) noexcept
      {
         if (pos >= end || end > src.size())
            return codec_fail("pssz: bitlist span out of bounds",
                              static_cast<std::uint32_t>(pos), "pssz");
         std::size_t span = end - pos;
         std::size_t last = span;
         if (static_cast<std::uint8_t>(src[end - 1]) == 0)
            return codec_fail("pssz: bitlist missing delimiter",
                              static_cast<std::uint32_t>(pos), "pssz");
         std::uint8_t lb = static_cast<std::uint8_t>(src[pos + last - 1]);
         int hi = 31 - __builtin_clz(static_cast<unsigned int>(lb));
         std::size_t bits =
            (last - 1) * 8 + static_cast<std::size_t>(hi);
         if (bits > MaxN)
            return codec_fail("pssz: bitlist bit_count exceeds bound",
                              static_cast<std::uint32_t>(pos), "pssz");
         return codec_ok();
      }

      template <std::size_t W, Record T, std::size_t... Is>
      codec_status validate_record(std::span<const char> src, std::size_t pos,
                                   std::size_t end, std::index_sequence<Is...> seq,
                                   std::size_t depth, bool* has_unknown) noexcept
      {
         using R = record_traits<T>;
         constexpr auto layout = record_layout<W, T>(seq);
         std::array<::psio::detail::pssz_field_span, R::member_count> spans{};
         bool unknown = false;
         auto status = ::psio::detail::pssz_record_spans<W>(
            src, pos, end, ::psio::is_dwnc_v<T>, layout, spans, unknown);
         if (!status.ok()) return status;
         if (has_unknown) *has_unknown |= unknown;
         (([&] {
            if (!status.ok() || !spans[Is].present) return;
            using F = typename R::template member_type<Is>;
            using eff = typename record_member_annotations<T, Is>::value_t;
            if constexpr (::psio::has_as_override_v<eff>)
            {
               if (depth == kMaxValidationDepth)
                  status = codec_fail("pssz: max depth exceeded", 0, "pssz");
               else
               {
                  using Proj = ::psio::adapter<std::remove_cvref_t<F>, ::psio::adapter_tag_of_t<eff>>;
                  status = Proj::validate(src.subspan(spans[Is].begin, spans[Is].end - spans[Is].begin));
               }
            }
            else status = validate_value<W, F>(src, spans[Is].begin, spans[Is].end,
                                               depth + 1, has_unknown);
         }()), ...);
         return status;
      }

      template <std::size_t W, typename T>
      codec_status validate_value(std::span<const char> src, std::size_t pos,
                                  std::size_t end,
                                  std::size_t depth, bool* has_unknown) noexcept
      {
         if (pos > end || end > src.size())
            return codec_fail("pssz: value span out of bounds", 0, "pssz");
         if (depth > kMaxValidationDepth)
            return codec_fail("pssz: max depth exceeded",
                              static_cast<std::uint32_t>(pos), "pssz");
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            return Proj::validate(std::span<const char>(
               src.data() + pos,
               (end > pos) ? (end - pos) : 0));
         }
         else if constexpr (is_psio_box_v<T>)
            return validate_value<W, typename is_psio_box<T>::elem>(src, pos, end, depth, has_unknown);
         else if constexpr (is_fixed_v<T> && !Record<T>)
         {
            if (end - pos != fixed_size_of<T>())
               return codec_fail("pssz: fixed primitive span mismatch", 0, "pssz");
            if constexpr (std::is_same_v<T, bool>)
            {
               if (static_cast<unsigned char>(src[pos]) > 1)
                  return codec_fail("pssz: bool is not 0/1", 0, "pssz");
            }
            else if constexpr (is_std_array_v<T>)
               return validate_array_payload<W, typename T::value_type, std::tuple_size_v<T>>(
                  src, pos, end, depth, has_unknown);
            else if constexpr (is_bitvector_v<T>)
            {
               if constexpr (T::size_value % 8 != 0)
                  if ((static_cast<unsigned char>(src[end - 1]) >> (T::size_value % 8)) != 0)
                     return codec_fail("pssz: nonzero unused bitvector bits", 0, "pssz");
            }
            return codec_ok();
         }
         else if constexpr (is_bitlist_v<T>)
            return validate_bitlist_payload<W, T::max_size_value>(src, pos, end);
         else if constexpr (std::is_same_v<T, std::string>)
            return ::psio::utf8_spec::validate(src.subspan(pos, end - pos));
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            return validate_array_payload<W, E, N>(src, pos, end, depth, has_unknown);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            return validate_vector_payload<W, E>(src, pos, end, depth, has_unknown);
         }
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            if (pos > end || end > src.size())
               return codec_fail("pssz: optional span out of bounds",
                                 static_cast<std::uint32_t>(pos), "pssz");
            if constexpr (positive_min_size<W, V>())
            {
               // Positive-minimum payloads cannot collide with empty None.
               const std::size_t span = end - pos;
               if (span == 0)
                  return codec_ok();
               return validate_value<W, V>(src, pos, end, depth + 1, has_unknown);
            }
            else
            {
               // A possibly empty payload needs a selector byte.
               if (pos == end)
                  return codec_fail("pssz: optional<var> missing selector",
                                    static_cast<std::uint32_t>(pos), "pssz");
               const std::uint8_t sel =
                  static_cast<std::uint8_t>(src[pos]);
               if (sel == 0)
               {
                  if (end - pos != 1)
                     return codec_fail("pssz: None has trailing bytes",
                                       static_cast<std::uint32_t>(pos), "pssz");
                  return codec_ok();
               }
               if (sel != 1)
                  return codec_fail("pssz: optional selector not 0/1",
                                    static_cast<std::uint32_t>(pos), "pssz");
               return validate_value<W, V>(src, pos + 1, end, depth + 1, has_unknown);
            }
         }
         else if constexpr (is_std_variant_v<T>)
         {
            if (pos >= end || end > src.size())
               return codec_fail("pssz: variant span out of bounds",
                                 static_cast<std::uint32_t>(pos), "pssz");
            const std::uint8_t sel =
               static_cast<std::uint8_t>(src[pos]);
            constexpr std::size_t NA = std::variant_size_v<T>;
            if (sel >= NA)
               return codec_fail("pssz: variant selector out of range",
                                 static_cast<std::uint32_t>(pos), "pssz");
            return [&]<std::size_t... Js>(std::index_sequence<Js...>) noexcept {
               codec_status err = codec_ok();
               (void)(((sel == Js)
                    ? (err = validate_value<
                                W,
                                std::variant_alternative_t<Js, T>>(
                          src, pos + 1, end, depth + 1, has_unknown),
                       true)
                    : false) ||
                ...);
               return err;
            }(std::make_index_sequence<NA>{});
         }
         else if constexpr (Record<T>)
         {
            using R = record_traits<T>;
            return validate_record<W, T>(
               src, pos, end, std::make_index_sequence<R::member_count>{},
               depth, has_unknown);
         }
         else
         {
            return codec_fail("pssz: unsupported type in validate",
                              static_cast<std::uint32_t>(pos), "pssz");
         }
      }

      template <std::size_t W, typename T>
      codec_status validate_exact_value(std::span<const char> bytes) noexcept
      {
         auto status = check_max_dynamic<T>(bytes);
         if (!status.ok()) return status;
         bool unknown = false;
         status = validate_value<W, T>(bytes, 0, bytes.size(), 0, &unknown);
         if (!status.ok()) return status;
         if (unknown) return codec_fail("pssz: unknown fields in exact validation", 0, "pssz");
#if defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
         try
#endif
         {
            T decoded = decode_value<W, T>(bytes, 0, bytes.size());
            if constexpr (::psio::Reflected<T>)
            {
               status = ::psio::validate_specs_on_value(decoded);
               if (!status.ok()) return status;
            }
            const auto size = size_of_v<W>(decoded);
            if (size != bytes.size())
               return codec_fail("pssz: noncanonical encoding", 0, "pssz");
            std::vector<char> canonical(size);
            ::psio::fast_buf_stream sink(canonical.data(), canonical.size());
            encode_value<W>(decoded, sink);
            if (!std::equal(canonical.begin(), canonical.end(), bytes.begin()))
               return codec_fail("pssz: noncanonical encoding", 0, "pssz");
            return codec_ok();
         }
#if defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
         catch (...) { return codec_fail("pssz: exact validation decode failed", 0, "pssz"); }
#endif
      }

      template <std::size_t W, typename T>
      void validate_or_throw_value(std::span<const char> src, std::size_t pos, std::size_t end)
      {
         auto status = validate_value<W, T>(src, pos, end);
         if (!status.ok()) ::psio::codec_abort(status.error());
      }

   }  // namespace detail::pssz_impl

   // ── Format tags ───────────────────────────────────────────────────────────

   // ── Width selection ───────────────────────────────────────────────────
   //
   // pssz is one format tag. Its internal offset width (1, 2, or 4
   // bytes) is selected at compile time from
   // `effective_max_dynamic_v<T>` (max_size.hpp), which composes:
   //   - `max_encoded_size<T>()` — the bound inferred from per-field
   //     `length_bound` annotations
   //   - `max_dynamic_data_v<T>` — the explicit type-level
   //     `maxDynamicData(N)` cap, when present
   // The explicit cap wins downward only — a tighter cap can narrow
   // the offset; a looser cap cannot widen past the inferred bound.
   //
   //   eff ≤ 0xff   → W = 1
   //   eff ≤ 0xffff → W = 2
   //   else / unbounded → W = 4
   //
   // Narrower widths kick in automatically when a record's variable
   // fields carry `length_bound` annotations OR when the schema author
   // asserts an explicit cap via `maxDynamicData(N)` in PSIO_REFLECT.

   template <typename T>
   consteval std::size_t auto_pssz_width() noexcept
   {
      constexpr auto m = ::psio::effective_max_dynamic_v<T>;
      if constexpr (m.has_value())
      {
         if constexpr (*m <= 0xFFu)
            return 1;
         else if constexpr (*m <= 0xFFFFu)
            return 2;
         else
            return 4;
      }
      else
      {
         return 4;
      }
   }

   template <typename T>
   inline constexpr std::size_t auto_pssz_width_v = auto_pssz_width<T>();

   // Explicit-width tag — `psio::pssz_<1|2|4>`. Not on the public
   // surface; retained so byte-parity regressions vs. v1's
   // `frac_format_pssz{8,16,32}` can name the exact width at the call
   // site. Production code uses `psio::pssz` which picks W via
   // `auto_pssz_width_v<T>`.
   template <std::size_t W>
   struct pssz_ : format_tag_base<pssz_<W>>
   {
      static constexpr std::size_t offset_width = W;
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), pssz_<W>,
                             const T& v, std::vector<char>& sink)
      {
         auto status = detail::pssz_impl::check_encode_depth<W>(v);
         if (!status.ok()) ::psio::codec_abort(status.error());
         const std::size_t total = detail::pssz_impl::size_of_v<W>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         const std::size_t orig = sink.size();
         sink.resize(orig + total);
         ::psio::fast_buf_stream fbs(sink.data() + orig, total);
         detail::pssz_impl::encode_value<W>(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          pssz_<W>, const T& v)
      {
         auto status = detail::pssz_impl::check_encode_depth<W>(v);
         if (!status.ok()) ::psio::codec_abort(status.error());
         const std::size_t total = detail::pssz_impl::size_of_v<W>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         std::vector<char> out(total);
         ::psio::fast_buf_stream fbs(out.data(), total);
         detail::pssz_impl::encode_value<W>(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), pssz_<W>, T*,
                          std::span<const char> bytes)
      {
         return detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of),
                                    pssz_<W>, const T& v)
      {
         return detail::pssz_impl::size_of_v<W>(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     pssz_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = detail::pssz_impl::check_max_dynamic<T>(bytes);
             !st.ok())
            return st;
         return detail::pssz_impl::validate_value<W, T>(bytes, 0,
                                                        bytes.size());
      }

      template <typename T>
      friend void tag_invoke(decltype(::psio::validate_or_throw<T>),
                             pssz_<W>, T*,
                             std::span<const char> bytes)
      {
         detail::pssz_impl::enforce_max_dynamic<T>(bytes.size());
         detail::pssz_impl::validate_or_throw_value<W, T>(
            bytes, 0, bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     pssz_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::pssz_impl::validate_exact_value<W, T>(bytes);
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           pssz_<W>, T*,
                                           std::span<const char> bytes) noexcept
      {
         return std::make_unique<T>(
            detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size()));
      }
   };

   // Internal aliases — used by byte-parity tests only. Not part of
   // the public surface; do not reference from production code.
   using pssz8  = pssz_<1>;
   using pssz16 = pssz_<2>;
   using pssz32 = pssz_<4>;

   struct pssz : format_tag_base<pssz>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), pssz,
                             const T& v, std::vector<char>& sink)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         auto status = detail::pssz_impl::check_encode_depth<W>(v);
         if (!status.ok()) ::psio::codec_abort(status.error());
         const std::size_t total = detail::pssz_impl::size_of_v<W>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         const std::size_t orig = sink.size();
         sink.resize(orig + total);
         ::psio::fast_buf_stream fbs(sink.data() + orig, total);
         detail::pssz_impl::encode_value<W>(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          pssz, const T& v)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         auto status = detail::pssz_impl::check_encode_depth<W>(v);
         if (!status.ok()) ::psio::codec_abort(status.error());
         const std::size_t total = detail::pssz_impl::size_of_v<W>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         std::vector<char> out(total);
         ::psio::fast_buf_stream fbs(out.data(), total);
         detail::pssz_impl::encode_value<W>(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), pssz, T*,
                          std::span<const char> bytes)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of),
                                    pssz, const T& v)
      {
         return detail::pssz_impl::size_of_v<auto_pssz_width_v<T>>(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     pssz, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = detail::pssz_impl::check_max_dynamic<T>(bytes);
             !st.ok())
            return st;
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return detail::pssz_impl::validate_value<W, T>(bytes, 0,
                                                        bytes.size());
      }

      template <typename T>
      friend void tag_invoke(decltype(::psio::validate_or_throw<T>),
                             pssz, T*, std::span<const char> bytes)
      {
         detail::pssz_impl::enforce_max_dynamic<T>(bytes.size());
         constexpr std::size_t W = auto_pssz_width_v<T>;
         detail::pssz_impl::validate_or_throw_value<W, T>(
            bytes, 0, bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     pssz, T*,
                                     std::span<const char> bytes) noexcept
      {
         constexpr auto W = auto_pssz_width_v<T>;
         return detail::pssz_impl::validate_exact_value<W, T>(bytes);
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           pssz, T*,
                                           std::span<const char> bytes) noexcept
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return std::make_unique<T>(
            detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size()));
      }
   };

   template <typename T, std::size_t W>
   [[nodiscard]] codec_status validate_pssz_compatible(
      pssz_<W>, std::span<const char> bytes, bool& has_unknown) noexcept
   {
      has_unknown = false;
      auto status = detail::pssz_impl::check_max_dynamic<T>(bytes);
      if (!status.ok()) return status;
      bool unknown = false;
      status = detail::pssz_impl::validate_value<W, T>(bytes, 0, bytes.size(), 0, &unknown);
      if (status.ok()) has_unknown = unknown;
      return status;
   }

   template <typename T>
   [[nodiscard]] codec_status validate_pssz_compatible(
      pssz, std::span<const char> bytes, bool& has_unknown) noexcept
   { return validate_pssz_compatible<T>(pssz_<auto_pssz_width_v<T>>{}, bytes, has_unknown); }

   template <typename T, typename Fmt>
   [[nodiscard]] codec_status decode_pssz_checked(
      Fmt format, std::span<const char> bytes, T& out, bool& has_unknown)
   {
      auto status = validate_pssz_compatible<T>(format, bytes, has_unknown);
      if (!status.ok()) return status;
      out = ::psio::decode<T>(format, bytes);
      return codec_ok();
   }

}  // namespace psio
