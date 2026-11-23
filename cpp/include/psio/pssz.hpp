#pragma once
//
// psio/pssz.hpp — pSSZ (PsiSSZ) format tag.
//
// Successor to the legacy fracpack prototype, which is retained for benchmarks.
//
// pSSZ is a variable-width variant of SSZ — offsets and container
// headers use 1, 2, or 4 bytes rather than always 4. The width is
// picked automatically per type by `auto_pssz_width_v<T>` from
// the type's declared `maxDynamicData` budget, or the conservative bound
// from `max_encoded_size<T>()` when no budget is declared (see max_size.hpp):
//
//   max ≤ 0xff   → W = 1
//   max ≤ 0xffff → W = 2
//   else         → W = 4
//
// A declared budget is authoritative even when current fields need less space.
// Otherwise, `length_bound{.max = N}` annotations bound the inferred width.
// Unbounded types fall back to W = 4. Each nested value retains its own type's
// width; the enclosing record/container uses its own width for its offset table.
// That includes a list: a std::vector bounded by its member's annotations, a
// psio::bounded, a map or a set whose count and elements are bounded writes its
// per-element offset table 1 or 2 bytes wide when its own bound allows it
// (docs/pssz-spec.md 3.2).
//
// Only one public tag: `psio::pssz`. The width choice is a compile-
// time detail of the encode/decode path, not a user-facing knob.
//
// `psio::pssz_<W>` exists as an explicit-width tag for
// byte-parity regression tests against v1; production code should
// always use `pssz` and let width selection happen automatically.
//
// Runtime values never widen their type's offsets. Encoding rejects an offset
// that cannot be represented, and automatic mode enforces declared budgets.
// Annotation-only types require a consumer ABI manifest to freeze their inferred
// widths across schema changes; inference cannot predict future fields.

#include <psio/cpo.hpp>
#include <psio/detail/nested_fwd.hpp>
#include <psio/detail/assoc.hpp>
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
#include <psio/time.hpp>
#include <psio/transparent.hpp>
#include <psio/validate_strict_walker.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
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

   template <typename T>
   consteval std::size_t auto_pssz_width() noexcept;

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
      // The first failure of a sizing pass run in checked mode (see
      // size_fail below).
      struct size_check
      {
         const char* failure = nullptr;
      };
      // P is the failure policy of a sizing pass (see size_fail below):
      // std::nullptr_t fails fast, size_check* records the first failure.
      template <typename T, typename P = std::nullptr_t>
      inline void enforce_max_dynamic(std::size_t total, P check = nullptr)
      {
         constexpr auto cap = ::psio::max_dynamic_data_v<T>;
         if constexpr (cap.has_value())
         {
            if (total > *cap) [[unlikely]]
            {
               if constexpr (!std::is_same_v<P, std::nullptr_t>)
               {
                  if (!check->failure)
                     check->failure = "pssz: encoded size exceeds maxDynamicData cap";
                  return;
               }
#if defined(__cpp_exceptions) && defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
               throw codec_exception{codec_error{
                  "pssz: encoded size exceeds maxDynamicData cap",
                  static_cast<std::uint32_t>(total), "pssz"}};
#else
               std::abort();
#endif
            }
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

      // psio::nested<T> (nested.hpp): a byte list holding the encoding of a
      // T. Variable-size, unbounded, minimum size 0, like vector<u8>.
      using ::psio::detail::is_nested;
      using ::psio::detail::is_nested_v;
      // ── Maps and sets (detail/pssz_assoc.hpp) ─────────────────────────
      //
      // A map / set is the list of its entries / elements in canonical
      // order; each ladder below dispatches here.
      using ::psio::detail::is_assoc_v;
      struct validate_flags;
      struct encode_preflight;
      template <typename T>
      codec_status assoc_check_encode(const T& value, std::size_t depth, encode_preflight bounds);
      template <std::size_t W, bool Auto, typename T, typename A, typename P>
      std::size_t assoc_size(const T& v, P check);
      template <std::size_t W, bool Auto, typename T, typename Sink, typename A>
      void assoc_encode(const T& v, Sink& s);
      template <std::size_t W, bool Auto, typename T, typename A>
      T assoc_decode(std::span<const char> src, std::size_t pos, std::size_t end);
      template <std::size_t W, bool Auto, typename T, typename A>
      codec_status assoc_validate(std::span<const char> src, std::size_t pos, std::size_t end,
                                  std::size_t depth, validate_flags* flags) noexcept;
      template <typename Old, typename New>
      consteval bool assoc_evolves() noexcept;

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
      // Plain C++ enums (`enum` and `enum class`) are their underlying
      // integer on the wire: sizeof(underlying) little-endian bytes, exactly
      // as the legacy format packed them. An enum is never a Record, even
      // if it is reflected, and an enum routed through a binary adapter is
      // the adapter's payload instead (see the adapter specialization).
      template <typename T>
         requires(std::is_enum_v<T> &&
                  !::psio::has_adapter_v<T, ::psio::binary_category>)
      struct is_fixed<T> : std::true_type
      {
      };
      // A duration / time point is its tick count: the Rep integer
      // (time.hpp), fixed width, little-endian.
      template <::psio::TimeValue T>
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

      // Positional tuples are encoded as extensible records: the same
      // header, offset slots and optional-suffix rules as a reflected
      // struct, addressed by index instead of by member pointer.
      template <typename T>
      struct is_std_tuple : std::false_type {};
      template <typename... Ts>
      struct is_std_tuple<std::tuple<Ts...>> : std::true_type {};
      // A std::pair is the 2-tuple of its members (a map's value_type,
      // pair<const K, V>, included).
      template <typename A, typename B>
      struct is_std_tuple<std::pair<A, B>> : std::true_type {};
      // A tuple carrying element annotations and a cap (wrappers.hpp): the
      // same positional record, read with those annotations.
      template <typename Meta, typename... Ts>
      struct is_std_tuple<::psio::annotated_tuple<Meta, Ts...>> : std::true_type {};
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

      template <typename Meta, typename... Ts>
      struct record_traits<::psio::annotated_tuple<Meta, Ts...>>
         : record_traits<std::tuple<Ts...>>
      {
      };

      template <typename A, typename B>
      struct record_traits<std::pair<A, B>>
      {
         static constexpr std::size_t member_count = 2;
         template <std::size_t I>
         using member_type = std::remove_const_t<std::tuple_element_t<I, std::pair<A, B>>>;
      };

      template <typename T, std::size_t I, bool = is_std_tuple_v<T>>
      struct record_member_annotations
      {
         using R = record_traits<T>;
         using F = typename R::template member_type<I>;
         using value_t = typename ::psio::effective_annotations_for<
            T, F, R::template member_pointer<I>>::value_t;
         static constexpr value_t value = ::psio::effective_annotations_for<
            T, F, R::template member_pointer<I>>::value;
      };
      // A plain tuple / pair element has no annotation of its own; an
      // annotated tuple (psio::annotated_tuple: a method's argument tuple)
      // carries one per element.
      template <typename T, std::size_t I>
      constexpr auto tuple_member_specs() noexcept
      {
         if constexpr (requires { typename T::psio_annotated_tuple; })
            return ::psio::detail::ann::at_specs<I>(T::psio_annotations);
         else
            return std::tuple<>{};
      }
      template <typename T, std::size_t I>
      struct record_member_annotations<T, I, true>
      {
         using value_t = std::remove_cvref_t<decltype(tuple_member_specs<T, I>())>;
         static constexpr value_t value = tuple_member_specs<T, I>();
      };

      // ── Annotation carriers (detail/annotation_tree.hpp) ───────────────
      //
      // Every walk below takes, after the value type, the carrier of the
      // annotations of the position it is at: no_ann for a bare position
      // (the default, and what every position without an annotation gets),
      // the member's effective annotations for a record member, and what
      // element(...) / keys(...) / values(...) / at<I>(...) state for the
      // positions below one. The carrier selects the offset width of a
      // nested array, tuple, pair or map entry, exactly as the equivalent
      // wrapper types (psio::bounded, psio::utf8_string) would, and the
      // validator checks its length_bound / utf8 at each position.
      namespace ann = ::psio::detail::ann;
      using ann::no_ann;
      // A record member's carrier: its effective annotations, and the
      // record's explicit maxDynamicData as the cap of the positions beneath.
      template <typename T, std::size_t I>
      struct member_raw
      {
         using value_t = typename record_member_annotations<T, I>::value_t;
         static constexpr value_t     value = record_member_annotations<T, I>::value;
         static constexpr std::size_t cap   = [] {
            if constexpr (::psio::max_dynamic_data_v<T>.has_value())
               return *::psio::max_dynamic_data_v<T>;
            else
               return ann::no_cap;
         }();
      };
      template <typename T, std::size_t I>
      using member_ann = typename ann::normalized<member_raw<T, I>>::type;
      // The carrier of field I of record T reached under carrier A: a
      // reflected record's (or an annotated tuple's) own member annotation;
      // for a plain tuple / pair, what at<I>(...) states in A.
      template <typename T, typename A, std::size_t I>
      struct field_ann_of
      {
         using type = member_ann<T, I>;
      };
      template <typename T, typename A, std::size_t I>
         requires(is_std_tuple_v<T> && !requires { typename T::psio_annotated_tuple; })
      struct field_ann_of<T, A, I>
      {
         using type = ann::at_ann<A, I>;
      };
      template <typename T, typename A, std::size_t I>
      using field_ann = typename field_ann_of<T, A, I>::type;

      template <typename T, typename A>
      consteval std::size_t width_for() noexcept;

      template <std::size_t I, typename T>
      decltype(auto) record_member(T&& value) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (is_std_tuple_v<U>)
            return std::get<I>(std::forward<T>(value));
         else
            return std::forward<T>(value).*(record_traits<U>::template member_pointer<I>);
      }

      // A transparent type (transparent.hpp) is its member, not a record.
      template <typename T>
      concept Record = (::psio::Reflected<T> && !std::is_enum_v<T> &&
                        !::psio::is_transparent_v<T>) ||
                       is_std_tuple_v<T>;

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

      // A transparent type is exactly as fixed as its member.
      template <typename T>
         requires(::psio::is_transparent_v<T> && !has_binary_adapter_v<T>)
      struct is_fixed<T> : is_fixed<::psio::transparent_inner_t<T>>
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
         else if constexpr (::psio::is_transparent_v<T>)
            return fixed_size_of<::psio::transparent_inner_t<T>>();
         else if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            return 16;
         else if constexpr (std::is_enum_v<T>)
            return sizeof(std::underlying_type_t<T>);
         else if constexpr (::psio::TimeValue<T>)
            return sizeof(::psio::time_rep_t<T>);
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

      // A fixed payload of zero bytes: its span must be empty, a vector of it
      // cannot carry a count, and an optional of it needs a selector (see
      // optional_uses_selector_v below).
      template <typename T>
      inline constexpr bool is_zero_fixed_v = is_fixed_v<T> && fixed_size_of<T>() == 0;

      template <typename T>
      codec_status check_zero_fixed_span(std::span<const char> src, std::size_t pos,
                                         std::size_t end) noexcept
      {
         if constexpr (is_zero_fixed_v<T>)
            if (pos != end || end > src.size())
               return codec_fail("pssz: zero-size fixed payload must be empty", 0, "pssz");
         return codec_ok();
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

      // utf8_string<N>: a string whose bytes must be UTF-8, at most N bytes
      // (utf8_string<> is unbounded). On the wire it is a String (raw bytes
      // to the end of its span); unlike a plain std::string, which is raw
      // bytes, its UTF-8 and length constraints are part of the type and are
      // enforced by encode, decode and every validation mode.
      template <typename T>
      struct is_utf8_string : std::false_type {};
      template <std::size_t N>
      struct is_utf8_string<::psio::utf8_string<N>> : std::true_type {};
      template <typename T>
      inline constexpr bool is_utf8_string_v = is_utf8_string<T>::value;

      // A number whose every byte pattern is a value: validating one checks
      // its size and nothing else.
      template <typename T>
      inline constexpr bool leaf_number_v =
         (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) ||
         (std::is_enum_v<T> && !::psio::has_invariant_v<T>) ||
         std::is_same_v<T, ::psio::uint128> || std::is_same_v<T, ::psio::int128> ||
         std::is_same_v<T, ::psio::uint256>;

      // psio::bounded<E, N>: a std::vector<E> of at most N elements. On the
      // wire it is the list; N is checked on encode, validate and decode.
      template <typename T>
      inline constexpr bool is_bounded_v = ann::is_bounded<T>::value;

      template <typename T>
      inline constexpr bool is_bitvector_v = is_bitvector<T>::value;
      template <typename T>
      inline constexpr bool is_bitlist_v = is_bitlist<T>::value;

      // ── Optional-suffix evolution ─────────────────────────────────────────
      //
      // A compatible record revision appends only optional fields. Each one
      // occupies a W-byte offset slot, so the writer's fixed-region header
      // tells a reader how many slots were actually written: an older reader
      // skips unknown trailing slots, a newer reader synthesizes None for
      // slots the writer did not emit, and the encoder prunes trailing None
      // fields. DWNC records have no header and are never pruned or extended.
      // See docs/pssz-optional-evolution.md.

      // min_encoded_size(T) > 0 (docs/pssz-spec.md 3.4): whether every
      // encoding of T is at least one byte. This is the true minimum of the
      // encoding, independent of fixed-size classification: an extensible
      // record always has its header, a variant its index, a bitlist its
      // delimiter, a non-empty array of variable elements its offset table,
      // and an optional that itself uses a selector its selector byte. A
      // string, vector, zero-size DWNC record, zero-count array, an adapter
      // without a size contract, and an optional of a positive-minimum
      // payload (whose None is empty) can all encode to zero bytes.
      template <typename T>
      consteval bool positive_min_size()
      {
         if constexpr (is_psio_box_v<T>)
            return positive_min_size<typename is_psio_box<T>::elem>();
         else if constexpr (is_nested_v<T>)
            return false; // A byte list: the inner encoding may be empty.
         else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::pssz, T>)
            return false; // An adapter without a minimum-size contract may encode empty.
         else if constexpr (::psio::is_transparent_v<T>)
            return positive_min_size<::psio::transparent_inner_t<T>>();
         else if constexpr (is_std_optional_v<T>)
            return !positive_min_size<typename T::value_type>();
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

      // Optional presence (docs/pssz-spec.md 5.5). This is the single
      // predicate used by sizing, encoding, decoding, validation and views:
      //   min(V) > 0  → no selector: None is zero bytes, Some is encoding(V).
      //   min(V) == 0 → one selector byte: 00, or 01 followed by encoding(V),
      //                 because an empty span could not tell None from an
      //                 empty Some.
      template <typename V>
      inline constexpr bool optional_uses_selector_v = !positive_min_size<V>();

      // A field may be omitted from the fixed region only when it is a plain
      // std::optional (an `as<Tag>` override owns its own representation).
      template <Record T, std::size_t I>
      inline constexpr bool omittable_field_v =
         is_std_optional_v<typename record_traits<T>::template member_type<I>> &&
         !::psio::has_as_override_v<typename record_member_annotations<T, I>::value_t>;

      // Number of leading fields the encoder writes: everything up to and
      // including the last field that is not an absent trailing optional.
      //
      // Only the trailing run of optional fields can be pruned, so only those
      // are looked at: `required_field_count` is the compile-time floor (one
      // past the last field that is always written), and a record that does
      // not end in an optional has a constant count.
      template <Record T>
      consteval std::size_t required_field_count() noexcept
      {
         using R = record_traits<T>;
         if constexpr (::psio::is_dwnc_v<T>)
            return R::member_count;
         else
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               std::size_t count = 0;
               ((omittable_field_v<T, Is> ? void() : void(count = Is + 1)), ...);
               return count;
            }(std::make_index_sequence<R::member_count>{});
      }
      template <Record T>
      std::size_t emitted_field_count(const T& value) noexcept
      {
         constexpr std::size_t N = record_traits<T>::member_count;
         constexpr std::size_t F = required_field_count<T>();
         if constexpr (F == N)
            return N;
         else
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               // Trailing optionals, last first: the first present one ends
               // the walk.
               std::size_t count = F;
               (void)((record_member<N - 1 - Is>(value).has_value()
                          ? (count = N - Is, true)
                          : false) ||
                      ...);
               return count;
            }(std::make_index_sequence<N - F>{});
      }
      // The fixed-region bytes of the fields every value writes.
      template <std::size_t W, Record T>
      consteval std::size_t required_fixed_region() noexcept;

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

      // Whether the payload span of an optional<V> field encodes None.
      template <typename V>
      bool optional_span_is_none(std::span<const char> src, std::size_t begin,
                                 std::size_t end) noexcept
      {
         if constexpr (optional_uses_selector_v<V>)
            return begin >= end || src[begin] == 0;
         else
            return begin >= end;
      }

      // Pruning is mandatory: a value has exactly one encoding. A record
      // whose last written slot is an absent optional should have had that
      // slot pruned, so every reader rejects it. When the writer appended
      // slots this reader does not know, the reader's own trailing optional
      // is no longer the last slot and the rule cannot be evaluated for the
      // unknown types; such frames are accepted and reported as unknown.
      // DWNC records carry no header, are never pruned, and are exempt.
      template <Record T, std::size_t... Is>
      bool record_has_unpruned_tail(
         std::span<const char> src,
         const std::array<::psio::detail::pssz_field_span, sizeof...(Is)>& spans,
         bool has_unknown, std::index_sequence<Is...>) noexcept
      {
         if constexpr (::psio::is_dwnc_v<T>)
            return false;
         else
         {
            if (has_unknown) return false;
            bool unpruned = false;
            (([&] {
               if constexpr (omittable_field_v<T, Is>)
               {
                  using F = typename record_traits<T>::template member_type<Is>;
                  const bool last_written =
                     spans[Is].present &&
                     (Is + 1 >= sizeof...(Is) ||
                      !spans[(Is + 1) % sizeof...(Is)].present);
                  if (last_written &&
                      optional_span_is_none<typename F::value_type>(
                         src, spans[Is].begin, spans[Is].end))
                     unpruned = true;
               }
            }()), ...);
            return unpruned;
         }
      }

      // Encoder preflight: node depth, null Box and zero-stride lists are
      // reported before any byte is sized or appended. Depth counting matches
      // the validator: root 0, each present field / element / Some payload /
      // variant payload adds one, a transparent Box adds none.
      //
      // A plain std::string is raw bytes and is not checked here; the
      // utf8_string<N> wrapper must be valid UTF-8 to be encoded.
      //
      // `bounds`: also reject a record member outside its declared
      // length_bound and a bitlist over its capacity, so the encoder never
      // writes a frame its own validator rejects. Tests that need such a
      // frame pass false.
      //
      // The preflight also runs the invariant (invariant.hpp) of every value
      // it walks, innermost first: check_encode_depth_impl is the shape walk
      // of check_encode_shape followed by the type's own check, and every
      // nested value goes through it.
      //
      // What the preflight checks beyond depth, null Box and UTF-8. It is
      // passed down the walk unchanged (as `bounds`, and tested as one: it
      // converts to whether bounds are checked).
      struct encode_preflight
      {
         bool bounds     = true;  // member length_bound, bitlist capacity
         bool invariants = true;  // type invariants
         constexpr encode_preflight(bool check_bounds = true,
                                    bool check_invariants = true) noexcept
            : bounds(check_bounds), invariants(check_invariants)
         {
         }
         constexpr explicit operator bool() const noexcept { return bounds; }
      };

      template <typename T>
      codec_status check_encode_shape(const T& value, std::size_t depth,
                                      encode_preflight bounds);

      template <typename T>
      codec_status check_encode_depth_impl(const T& value, std::size_t depth,
                                           encode_preflight bounds = {})
      {
         if constexpr (::psio::has_invariant_v<T>)
         {
            if (auto status = check_encode_shape(value, depth, bounds); !status.ok())
               return status;
            return bounds.invariants ? ::psio::check_invariant(value) : codec_ok();
         }
         else
            return check_encode_shape(value, depth, bounds);
      }

      template <typename T>
      codec_status check_encode_shape(const T& value, std::size_t depth,
                                      encode_preflight bounds)
      {
         if (depth > kMaxValidationDepth)
            return codec_fail("pssz: max depth exceeded", 0, "pssz");
         if constexpr (is_psio_box_v<T>)
         {
            if (!value.value) return codec_fail("pssz: null Box", 0, "pssz");
            return check_encode_depth_impl(*value.value, depth, bounds);
         }
         else if constexpr (::psio::is_transparent_v<T> &&
                            !::psio::format_should_dispatch_adapter_v<::psio::pssz, T>)
            return check_encode_depth_impl(::psio::transparent_inner(value), depth, bounds);
         else if constexpr (is_nested_v<T>)
            return ::psio::detail::nested_impl::pssz_check_encode(value, depth);
         else if constexpr (is_assoc_v<T>)
            return assoc_check_encode(value, depth, bounds);
         else if constexpr (is_bitlist_v<T>)
         {
            if (bounds && value.size() > T::max_size_value)
               return codec_fail("pssz: bitlist bit_count exceeds bound", 0, "pssz");
         }
         else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::pssz, T>)
            return codec_ok();
         else if constexpr (is_std_optional_v<T>)
         {
            if (value) return check_encode_depth_impl(*value, depth + 1, bounds);
         }
         else if constexpr (is_bounded_v<T>)
         {
            if (bounds && value.size() > T::max_size_value)
               return codec_fail("pssz: bounded list exceeds its bound", 0, "pssz");
            return check_encode_shape(value.storage(), depth, bounds);
         }
         else if constexpr (is_std_array_v<T> || is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_std_vector_v<T> && is_fixed_v<E>)
               if constexpr (fixed_size_of<E>() == 0)
                  if (!value.empty())
                     return codec_fail("pssz: nonempty zero-size vector is unsupported", 0, "pssz");
            if constexpr (std::is_arithmetic_v<E> ||
                          (std::is_enum_v<E> && !::psio::has_invariant_v<E>))
            {
               // Leaf elements: one depth check covers all of them.
               if (!value.empty() && depth + 1 > kMaxValidationDepth)
                  return codec_fail("pssz: max depth exceeded", 0, "pssz");
            }
            else
               for (const auto& element : value)
               {
                  auto status = check_encode_depth_impl<E>(element, depth + 1, bounds);
                  if (!status.ok()) return status;
               }
         }
         else if constexpr (is_utf8_string_v<T>)
         {
            // The wrapper's constructor checks only the bound; the encoder
            // owns the UTF-8 check (and re-checks the bound defensively).
            if (value.size() > T::max_size_value)
               return codec_fail("pssz: utf8_string exceeds its bound", 0, "pssz");
            return ::psio::utf8_spec::validate(
               std::span<const char>(value.data(), value.size()));
         }
         else if constexpr (is_std_variant_v<T>)
            return std::visit([&](const auto& element) {
               return check_encode_depth_impl(element, depth + 1, bounds);
            }, value);
         else if constexpr (Record<T>)
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               codec_status status;
               const auto count = emitted_field_count(value);
               (([&] {
                  if (status.ok() && Is < count)
                  {
                     status = check_encode_depth_impl(record_member<Is>(value), depth + 1,
                                                      bounds);
                     using eff = typename record_member_annotations<T, Is>::value_t;
                     // The member's whole annotation tree: its own
                     // length_bound / utf8 and those of the positions below.
                     if constexpr (ann::has_tree_specs_v<eff> &&
                                   !::psio::has_as_override_v<eff>)
                        if (status.ok() && bounds)
                           status = ::psio::check_member_annotations(
                              record_member<Is>(value),
                              record_member_annotations<T, Is>::value,
                              "pssz: member length_bound violated", "pssz");
                  }
               }()), ...);
               (void)count;
               return status;
            }(std::make_index_sequence<record_traits<T>::member_count>{});
         return codec_ok();
      }

      // The width parameter is kept for source compatibility with callers of
      // the evolution line; the walk itself is width-independent.
      template <std::size_t W, typename T>
      codec_status check_encode_depth(const T& value, std::size_t depth = 0)
      {
         return check_encode_depth_impl(value, depth);
      }
      // ── Sizing ────────────────────────────────────────────────────────────
      //
      // For pSSZ, offsets cost W bytes. The shape remains the same as SSZ.

      [[noreturn]] inline void size_failure(const char* message)
      {
#if defined(__cpp_exceptions) && defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
         throw codec_exception{codec_error{message, 0, "pssz"}};
#else
         (void)message;
         std::abort();
#endif
      }
      // Canonical encodings carry no trailing data. A fixed-size value
      // (including a bare top-level scalar and a zero-size payload) must
      // occupy exactly its span on the fail-fast decode paths, as it does in
      // validate. Variable-size values own their whole span by definition.
      template <typename T>
      void enforce_exact_fixed_span(std::span<const char> src, std::size_t pos,
                                    std::size_t end)
      {
         if constexpr (is_fixed_v<T>)
            if (pos > end || end > src.size() || end - pos != fixed_size_of<T>())
               size_failure("pssz: fixed-size value does not match its span");
      }
      // Raise a status produced by a checked helper on a fail-fast path:
      // codec_exception natively, abort when exceptions are disabled.
      [[noreturn]] inline void status_failure(const codec_status& status)
      {
#if defined(__cpp_exceptions) && defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
         throw codec_exception{status.error()};
#else
         (void)status;
         std::abort();
#endif
      }
      // Encoder preflight (see check_encode_depth). Runs before sizing so a
      // rejected value never resizes or appends to the destination.
      template <typename T>
      void enforce_encodable(const T& value)
      {
         if (auto status = check_encode_depth_impl(value, 0); !status.ok())
            status_failure(status);
      }
      // What the sizing pass does when a value has no encoding (a size that
      // overflows, a payload past the static offset width, a nested record
      // over its maxDynamicData cap, ...). With no `size_check` it fails
      // fast: the plain encoder. With one it records the first failure and
      // carries on — the sizes computed after it are meaningless and the
      // caller discards them — so the checked encoder can report a status
      // without exceptions.
      //
      // The policy is a type, so the fail-fast pass carries no pointer and
      // tests none: `std::nullptr_t` (the default of every helper below)
      // raises, `size_check*` records.
      [[noreturn, gnu::cold]] inline void size_fail(std::nullptr_t, const char* message)
      {
         size_failure(message);
      }
      [[gnu::cold]] inline void size_fail(size_check* check, const char* message)
      {
         if (!check->failure)
            check->failure = message;
      }
      template <typename P = std::nullptr_t>
      inline std::size_t checked_add(std::size_t a, std::size_t b, P check = nullptr)
      {
         if (b > std::numeric_limits<std::size_t>::max() - a) [[unlikely]]
            size_fail(check, "pssz: encoded size overflow");
         return a + b;
      }
      template <typename P = std::nullptr_t>
      inline std::size_t checked_multiply(std::size_t a, std::size_t b, P check = nullptr)
      {
         if (a && b > std::numeric_limits<std::size_t>::max() / a) [[unlikely]]
            size_fail(check, "pssz: encoded size overflow");
         return a * b;
      }
      inline bool wire_span_fits(std::size_t size) noexcept
      {
         return size <= std::numeric_limits<std::uint32_t>::max();
      }
      template <typename P = std::nullptr_t>
      inline void enforce_wire_span(std::size_t size, P check = nullptr)
      {
         if (!wire_span_fits(size)) [[unlikely]]
            size_fail(check, "pssz: variable payload exceeds 32-bit address space");
      }
      template <std::size_t W, typename P = std::nullptr_t>
      typename width_info<W>::offset_t checked_offset(std::size_t value, P check = nullptr)
      {
         using O = typename width_info<W>::offset_t;
         if (value > std::numeric_limits<O>::max()) [[unlikely]]
            size_fail(check, "pssz: value exceeds the static offset width");
         return static_cast<O>(value);
      }
      // The payload bytes of one container, summed without a branch per
      // element: an overflow sets a flag that is tested once, at the end.
      // (A sum of sizes of values that exist in memory cannot overflow; an
      // adapter's packsize is the one input that could lie.)
      struct size_sum
      {
         std::size_t total    = 0;
         bool        overflow = false;
         void add(std::size_t n) noexcept
         {
            overflow |= __builtin_add_overflow(total, n, &total);
         }
      };
      // The checks of one container, once: the sum did not overflow, and
      // the largest offset it writes — `last_offset`, the start of its last
      // variable payload; offsets only grow — fits the offset width. With
      // 4-byte offsets the 32-bit span check of the total covers the latter.
      template <std::size_t W, typename P>
      inline void enforce_container(const size_sum& sum, std::size_t last_offset, P check)
      {
         if (sum.overflow) [[unlikely]]
            size_fail(check, "pssz: encoded size overflow");
         if constexpr (W < 4)
            (void)checked_offset<W>(last_offset, check);
         else
            (void)last_offset;
         enforce_wire_span(sum.total, check);
      }
      template <typename Sink>
      void skip_bytes(Sink& sink, std::size_t count)
      {
         // Existing sinks accept signed 32-bit skips. Split larger spans rather
         // than narrowing an otherwise representable u32 offset table.
         constexpr auto chunk = std::size_t(std::numeric_limits<std::int32_t>::max());
         while (count > chunk)
         {
            sink.skip(static_cast<std::int32_t>(chunk));
            count -= chunk;
         }
         sink.skip(static_cast<std::int32_t>(count));
      }
      template <typename T>
      consteval bool fixed_caps_valid()
      {
         if constexpr (is_fixed_v<T>)
            if constexpr (::psio::max_dynamic_data_v<T>.has_value())
               if constexpr (fixed_size_of<T>() > *::psio::max_dynamic_data_v<T>)
                  return false;
         if constexpr (Record<T>)
         {
            using R = record_traits<T>;
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return ((!is_fixed_v<typename R::template member_type<Is>> ||
                        fixed_caps_valid<typename R::template member_type<Is>>()) && ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else if constexpr (::psio::is_transparent_v<T>)
            return fixed_caps_valid<::psio::transparent_inner_t<T>>();
         else if constexpr (is_std_array_v<T>)
         {
            using E = typename T::value_type;
            return std::tuple_size<T>::value == 0 || !is_fixed_v<E> || fixed_caps_valid<E>();
         }
         else return true;
      }
      template <typename T, typename P = std::nullptr_t>
      void enforce_fixed_caps(P check = nullptr)
      {
         if constexpr (!fixed_caps_valid<T>())
            size_fail(check, "pssz: fixed payload exceeds a declared maxDynamicData budget");
      }

      template <std::size_t W, bool Auto = false, typename T, typename A = no_ann,
                typename P = std::nullptr_t>
      std::size_t size_of_v(const T& v, P check = nullptr);

      template <std::size_t W, bool Auto, typename T, typename A, typename P>
      std::size_t size_of_v_impl(const T& v, P check)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            return Proj::packsize(v);
         }
         else if constexpr (is_psio_box_v<T>)
            return size_of_v<W, Auto, typename is_psio_box<T>::elem, ann::cap_only<A>>(*v.value,
                                                                                    check);
         else if constexpr (::psio::is_transparent_v<T>)
            return size_of_v<W, Auto, ::psio::transparent_inner_t<T>, ann::cap_only<A>>(
               ::psio::transparent_inner(v), check);
         else if constexpr (is_nested_v<T>)
            return v.size();
         else if constexpr (is_assoc_v<T>)
            return assoc_size<W, Auto, T, A, P>(v, check);
         else if constexpr (is_bounded_v<T>)
            return size_of_v_impl<W, Auto, std::vector<typename T::value_type>, A, P>(
               v.storage(), check);
         else if constexpr (is_fixed_v<T>)
            return fixed_size_of<T>();
         else if constexpr (is_std_array_v<T>)
         {
            constexpr std::size_t N = std::tuple_size<T>::value;
            size_sum    sum{N * W, false};  // the offset table
            std::size_t last = sum.total;
            for (const auto& x : v)
            {
               last = sum.total;
               sum.add(size_of_v<W, Auto, typename T::value_type, ann::element_ann<A>>(x, check));
            }
            enforce_container<W>(sum, last, check);
            return sum.total;
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               if constexpr (is_zero_fixed_v<E>)
                  if (!v.empty()) size_fail(check, "pssz: nonempty zero-size vector is unsupported");
               if constexpr (Auto)
                  if (!v.empty()) enforce_fixed_caps<E>(check);
               return checked_multiply(v.size(), fixed_size_of<E>(), check);
            }
            else
            {
               size_sum    sum{checked_multiply(v.size(), W, check), false};  // the table
               std::size_t last = sum.total;
               for (const auto& x : v)
               {
                  last = sum.total;
                  sum.add(size_of_v<W, Auto, E, ann::element_ann<A>>(x, check));
               }
               enforce_container<W>(sum, last, check);
               return sum.total;
            }
         }
         else if constexpr (std::is_same_v<T, std::string> || is_utf8_string_v<T>)
            return v.size();
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            // Positive-minimum V needs no selector; a V that can encode to
            // zero bytes uses the 1-byte selector plus its payload.
            if constexpr (!optional_uses_selector_v<V>)
               return v.has_value() ? size_of_v<W, Auto, V, A>(*v, check) : 0;
            else
               return checked_add(1, v.has_value() ? size_of_v<W, Auto, V, A>(*v, check) : 0,
                                  check);
         }
         else if constexpr (is_bitlist_v<T>)
            return (v.size() + 8) / 8;
         else if constexpr (is_std_variant_v<T>)
         {
            std::size_t total = 1;
            if constexpr (std::is_same_v<A, no_ann>)
               std::visit([&](const auto& alt) { total = checked_add(total, size_of_v<W, Auto>(alt, check), check); },
                          v);
            else
               [&]<std::size_t... Js>(std::index_sequence<Js...>) {
                  (([&] {
                      if (v.index() == Js)
                         total = checked_add(
                            total,
                            size_of_v<W, Auto, std::variant_alternative_t<Js, T>,
                                      ann::at_ann<A, Js>>(std::get<Js>(v), check),
                            check);
                   }()),
                   ...);
               }(std::make_index_sequence<std::variant_size_v<T>>{});
            return total;
         }
         else if constexpr (Record<T>)
         {
            using R = record_traits<T>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               // Trailing absent optional fields are pruned from the fixed
               // region (never for DWNC), so the region size is a property
               // of the value, bounded above by the type's static layout.
               // The fields before the trailing optionals are always
               // written: their share of the region is a constant, and only
               // the trailing ones are tested against the count.
               constexpr std::size_t required = required_field_count<T>();
               const std::size_t     count    = emitted_field_count(v);
               std::size_t fixed_region       = required_fixed_region<W, T>();
               ((Is >= required && Is < count
                    ? fixed_region += pssz_member_fixed_size_full<W, T, Is>()
                    : std::size_t{0}), ...);
               // u{W} fixed_size header — DWNC types skip it.
               if constexpr (!::psio::is_dwnc_v<T>)
                  (void)checked_offset<W>(fixed_region, check);
               // The fixed region, then the payloads of the variable
               // fields; `last` is where the last of them starts.
               size_sum    sum{fixed_region, false};
               std::size_t last = fixed_region;
               (
                  ([&]
                   {
                      if constexpr (Is >= required)
                         if (Is >= count) return;
                      using F = typename R::template member_type<Is>;
                      using eff =
                         typename record_member_annotations<T, Is>::value_t;
                      constexpr bool override_v =
                         ::psio::has_as_override_v<eff>;
                      if constexpr (override_v)
                      {
                         // Member-level `as<Tag>` forces the field through
                         // the named adapter — a W-byte offset slot (in the
                         // fixed region) plus the adapter's payload.
                         using Tag = ::psio::adapter_tag_of_t<eff>;
                         using Proj = ::psio::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         last = sum.total;
                         sum.add(Proj::packsize(record_member<Is>(v)));
                      }
                      else if constexpr (!is_fixed_v<F>)
                      {
                         last = sum.total;
                         sum.add(size_of_v<W, Auto, F, field_ann<T, A, Is>>(
                            record_member<Is>(v), check));
                      }
                   }()),
                  ...);
               (void)count;
               enforce_container<W>(sum, last, check);
               return sum.total + (::psio::is_dwnc_v<T> ? 0 : W);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            PSIO_REJECT_MULTI_ASSOC("psio::pssz", T);
            static_assert(sizeof(T) == 0,
                          "psio::pssz: unsupported type in size_of_v");
            return 0;
         }
      }

      template <std::size_t W, Record T>
      consteval std::size_t required_fixed_region() noexcept
      {
         return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (pssz_member_fixed_size_full<W, T, Is>() + ... + std::size_t{0});
         }(std::make_index_sequence<required_field_count<T>()>{});
      }

      template <std::size_t LegacyW, bool Auto, typename T, typename A, typename P>
      std::size_t size_of_v(const T& value, P check)
      {
         constexpr std::size_t W = [] {
            if constexpr (Auto) return width_for<T, A>();
            else return LegacyW;
         }();
         if constexpr (Auto) enforce_fixed_caps<T>(check);
         const auto total = size_of_v_impl<W, Auto, T, A, P>(value, check);
         if constexpr (Auto) enforce_max_dynamic<T>(total, check);
         return total;
      }

      // ── Encoding ──────────────────────────────────────────────────────────

      using sink_t = std::vector<char>;

      template <std::size_t W, typename Sink>
      void write_offset(Sink& s, std::size_t pos, std::size_t value)
      {
         using O = typename width_info<W>::offset_t;
         O ov   = checked_offset<W>(value);
         s.rewrite(pos, &ov, W);
      }

      template <std::size_t LegacyW, bool Auto = false, typename T, typename Sink,
                typename A = no_ann>
      void encode_value(const T& v, Sink& s);

      // A list (std::vector, psio::bounded) whose offset table is W wide.
      // EA: the carrier of one element.
      template <std::size_t W, bool Auto, typename E, typename Sink, typename EA>
      [[gnu::always_inline]] inline void encode_list(const std::vector<E>& v, Sink& s)
      {
         if constexpr (is_fixed_v<E>)
         {
            if constexpr (is_zero_fixed_v<E>)
               if (!v.empty()) size_failure("pssz: nonempty zero-size vector is unsupported");
            // Bulk memcpy for arithmetic OR memcpy-layout records.
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               (Record<E> || ::psio::is_transparent_v<E>) &&
               std::is_trivially_copyable_v<E> && fixed_size_of<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               if (!v.empty())
                  s.write(v.data(), v.size() * sizeof(E));
            }
            else
            {
               for (const auto& x : v)
                  encode_value<W, Auto>(x, s);
            }
         }
         else
         {
            const std::size_t n     = v.size();
            const std::size_t table = s.written();
            skip_bytes(s, checked_multiply(n, W));
            for (std::size_t i = 0; i < n; ++i)
            {
               write_offset<W>(s, table + i * W,
                               s.written() - table);
               encode_value<W, Auto, E, Sink, EA>(v[i], s);
            }
         }
      }

      // Sink-templated encoder. Top-level tag_invoke pre-sizes the
      // output buffer via size_of_v then walks once with
      // fast_buf_stream — same algorithm v1's convert_to_pssz uses.
      template <std::size_t LegacyW, bool Auto, typename T, typename Sink, typename A>
      void encode_value(const T& v, Sink& s)
      {
         constexpr std::size_t W = [] {
            if constexpr (Auto) return width_for<T, A>();
            else return LegacyW;
         }();
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
            encode_value<W, Auto, typename is_psio_box<T>::elem, Sink, ann::cap_only<A>>(*v.value,
                                                                                         s);
            return;
         }
         else if constexpr (::psio::is_transparent_v<T>)
         {
            encode_value<W, Auto, ::psio::transparent_inner_t<T>, Sink, ann::cap_only<A>>(
               ::psio::transparent_inner(v), s);
            return;
         }
         else if constexpr (is_nested_v<T>)
         {
            // The bytes, as they are: a nested value holds a valid encoding.
            if (v.size() != 0)
               s.write(v.bytes().data(), v.size());
         }
         else if constexpr (is_assoc_v<T>)
            assoc_encode<W, Auto, T, Sink, A>(v, s);
         else if constexpr (is_bounded_v<T>)
            // The list, with this type's own table width.
            encode_list<W, Auto, typename T::value_type, Sink, ann::element_ann<A>>(v.storage(),
                                                                                    s);
         else if constexpr (std::is_same_v<T, bool>)
            s.write(v ? '\x01' : '\x00');
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            s.write(v.limb, 32);
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            s.write(&v, 16);
         else if constexpr (std::is_enum_v<T>)
         {
            // The underlying integer, little-endian.
            const auto raw = static_cast<std::underlying_type_t<T>>(v);
            s.write(&raw, sizeof(raw));
         }
         else if constexpr (::psio::TimeValue<T>)
         {
            // The tick count, little-endian.
            const auto raw = ::psio::time_ticks(v);
            s.write(&raw, sizeof(raw));
         }
         else if constexpr (std::is_arithmetic_v<T>)
            s.put(v);
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
            {
               for (const auto& x : v)
                  encode_value<W, Auto>(x, s);
            }
            else
            {
               const std::size_t table = s.written();
               skip_bytes(s, checked_multiply(N, W));
               for (std::size_t i = 0; i < N; ++i)
               {
                  write_offset<W>(s, table + i * W,
                                  s.written() - table);
                  encode_value<W, Auto, E, Sink, ann::element_ann<A>>(v[i], s);
               }
            }
         }
         else if constexpr (is_std_vector_v<T>)
            encode_list<W, Auto, typename T::value_type, Sink, ann::element_ann<A>>(v, s);
         else if constexpr (std::is_same_v<T, std::string> || is_utf8_string_v<T>)
            s.write(v.data(), v.size());
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            // pSSZ optional selector rule (matches v1):
            //   min_encoded_size(V) > 0 → no selector;
            //     the byte-span length (0 vs nonzero) disambiguates.
            //   min_encoded_size(V) == 0 → 1-byte Union
            //     selector (0x00 = None, 0x01 + payload = Some).
            if constexpr (!optional_uses_selector_v<V>)
            {
               if (v.has_value())
                  encode_value<W, Auto, V, Sink, A>(*v, s);
               // None → no bytes appended.
            }
            else
            {
               if (v.has_value())
               {
                  s.write('\x01');
                  encode_value<W, Auto, V, Sink, A>(*v, s);
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
            if constexpr (std::is_same_v<A, no_ann>)
               std::visit(
                  [&](const auto& alt) { encode_value<W, Auto>(alt, s); }, v);
            else
               [&]<std::size_t... Js>(std::index_sequence<Js...>) {
                  (([&] {
                      if (v.index() == Js)
                         encode_value<W, Auto, std::variant_alternative_t<Js, T>, Sink,
                                      ann::at_ann<A, Js>>(std::get<Js>(v), s);
                   }()),
                   ...);
               }(std::make_index_sequence<std::variant_size_v<T>>{});
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
               // Only the leading `count` fields are written; trailing
               // absent optionals are pruned (see emitted_field_count).
               // (Only the trailing optionals are tested: see size_of_v_impl.)
               constexpr std::size_t required = required_field_count<T>();
               const std::size_t     count    = emitted_field_count(v);
               std::size_t fixed_region       = required_fixed_region<W, T>();
               ((Is >= required && Is < count
                    ? fixed_region += pssz_member_fixed_size_full<W, T, Is>()
                    : std::size_t{0}), ...);

               // u{W} fixed_size header — the forward-compat slot. DWNC
               // types skip it. Non-DWNC records always carry the header
               // so a reader with a different number of trailing optional
               // fields knows how many bytes of fixed region were written.
               if constexpr (!::psio::is_dwnc_v<T>)
               {
                  using O = typename width_info<W>::offset_t;
                  O hdr   = checked_offset<W>(fixed_region);
                  s.write(&hdr, W);
               }

               const std::size_t container_start = s.written();
               skip_bytes(s, fixed_region);
               std::size_t fixed_cursor = container_start;

               (
                  ([&]
                   {
                      if constexpr (Is >= required)
                         if (Is >= count) return;
                      using F = typename R::template member_type<Is>;
                      const auto& fref = record_member<Is>(v);
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
                         else if constexpr (std::is_arithmetic_v<F> ||
                                            std::is_enum_v<F>)
                         {
                            // sizeof(enum) == sizeof(its underlying type).
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
                               encode_value<W, Auto>(fref, vs);
                               s.rewrite(fixed_cursor, tmp.data(),
                                         tmp.size());
                            }
                         }
                         else
                         {
                            sink_t tmp;
                            ::psio::vector_stream vs{tmp};
                            encode_value<W, Auto>(fref, vs);
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
                            encode_value<W, Auto, F, Sink, field_ann<T, A, Is>>(fref, s);
                         }
                      }
                   }()),
                  ...);
               (void)count;
               (void)fixed_cursor;
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            PSIO_REJECT_MULTI_ASSOC("psio::pssz", T);
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

      template <std::size_t W, bool Auto, typename T, typename A = no_ann>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end);

      template <std::size_t W, bool Auto, Record T, typename A, std::size_t... Is>
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
      template <std::size_t LegacyW, bool Auto, typename T, typename A = no_ann>
      void decode_into(std::span<const char> src, std::size_t pos,
                       std::size_t end, T& out)
      {
         constexpr std::size_t W = [] {
            if constexpr (Auto) return width_for<T, A>();
            else return LegacyW;
         }();
         enforce_exact_fixed_span<T>(src, pos, end);
         if constexpr (Auto)
         {
            if (pos > end || end > src.size())
               size_failure("pssz: value span out of bounds");
            enforce_max_dynamic<T>(end - pos);
            enforce_fixed_caps<T>();
         }
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            out = Proj::decode(
               std::span<const char>(src.data() + pos, end - pos));
            return;
         }
         else if constexpr (::psio::is_transparent_v<T>)
            decode_into<W, Auto, ::psio::transparent_inner_t<T>, ann::cap_only<A>>(
               src, pos, end, ::psio::transparent_inner(out));
         else if constexpr (std::is_same_v<T, std::string>)
         {
            // pSSZ string: raw bytes from pos to end (no length).
            out.assign(src.data() + pos, src.data() + end);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (Auto && is_fixed_v<E>)
               if (end > pos) enforce_fixed_caps<E>();
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               (Record<E> || ::psio::is_transparent_v<E>) &&
               std::is_trivially_copyable_v<E> &&
               is_fixed_v<E> && fixed_size_of<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               // Alignment-safe — see detail/unaligned_iter.hpp.
               if (pos > end || (end - pos) % sizeof(E) != 0)
                  size_failure("pssz: vector span not a multiple of element size");
               const std::size_t n = (end - pos) / sizeof(E);
               psio::detail::assign_from_wire(out, src.data() + pos, n);
            }
            else
            {
               out = decode_value<W, Auto, T, A>(src, pos, end);
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
            record_decode_into<W, Auto, T, A>(
               src, pos, end, out,
               std::make_index_sequence<R::member_count>{});
         }
         else
         {
            out = decode_value<W, Auto, T, A>(src, pos, end);
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

      // EA: the carrier of one element.
      template <std::size_t W, bool Auto, typename T, std::size_t N, typename EA = no_ann>
      std::array<T, N> decode_array(std::span<const char> src, std::size_t pos,
                                    std::size_t end)
      {
         std::array<T, N> out{};
         if constexpr (N == 0)
            if (pos != end) size_failure("pssz: zero-count array payload must be empty");
         if constexpr (!is_fixed_v<T>) enforce_wire_span(end - pos);
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            for (std::size_t i = 0; i < N; ++i)
               out[i] = decode_value<W, Auto, T>(src, pos + i * esz,
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
               out[i] = decode_value<W, Auto, T, EA>(src, beg, fin);
            }
         }
         return out;
      }

      // Forward declaration — used by decode_vector to decode record
      // elements in-place. Definition appears later in this file.
      template <std::size_t W, bool Auto, Record T, typename A, std::size_t... Is>
      void record_decode_into(std::span<const char> src, std::size_t pos,
                              std::size_t end, T& out,
                              std::index_sequence<Is...>);

      // EA: the carrier of one element.
      template <std::size_t W, bool Auto, typename T, typename EA = no_ann>
      std::vector<T> decode_vector(std::span<const char> src, std::size_t pos,
                                   std::size_t end)
      {
         std::vector<T> out;
         if (pos >= end)
            return out;
         if constexpr (!is_fixed_v<T>) enforce_wire_span(end - pos);
         if constexpr (is_zero_fixed_v<T>)
            size_failure("pssz: nonempty zero-size vector is unsupported");
         else if constexpr (is_fixed_v<T>)
         {
            if constexpr (Auto) enforce_fixed_caps<T>();
            const std::size_t esz = fixed_size_of<T>();
            if ((end - pos) % esz != 0)
               size_failure("pssz: vector span not a multiple of element size");
            const std::size_t n   = (end - pos) / esz;
            // Bulk-memcpy fast path covers arithmetic primitives AND
            // memcpy-layout DWNC packed records (sizeof matches wire).
            constexpr bool is_arith =
               std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;
            constexpr bool is_memcpy_record =
               (Record<T> || ::psio::is_transparent_v<T>) &&
               std::is_trivially_copyable_v<T> && fixed_size_of<T>() == sizeof(T);
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
                  record_decode_into<W, Auto, T, EA>(
                     src, pos + i * esz, pos + (i + 1) * esz, out[i],
                     std::make_index_sequence<R::member_count>{});
            }
            else
            {
               out.reserve(n);
               for (std::size_t i = 0; i < n; ++i)
                  out.push_back(decode_value<W, Auto, T>(src, pos + i * esz,
                                                    pos + (i + 1) * esz));
            }
         }
         else
         {
            if (end - pos < W)
               return out;
            const std::uint32_t first = read_offset<W>(src, pos);
            const std::size_t   n     = first / W;
            // The offset table is read in place: no temporary copy of it
            // (a heap allocation per list decoded).
            const auto offset_at = [&](std::size_t i) {
               return read_offset<W>(src, pos + i * W);
            };
            if constexpr (Record<T>)
            {
               // In-place: resize then walk each element directly into
               // its slot, avoiding push_back's move-construct.
               out.resize(n);
               using R = record_traits<T>;
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::size_t beg = pos + offset_at(i);
                  const std::size_t fin =
                     (i + 1 < n) ? (pos + offset_at(i + 1)) : end;
                  record_decode_into<W, Auto, T, EA>(
                     src, beg, fin, out[i],
                     std::make_index_sequence<R::member_count>{});
               }
            }
            else
            {
               out.reserve(n);
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::size_t beg = pos + offset_at(i);
                  const std::size_t fin =
                     (i + 1 < n) ? (pos + offset_at(i + 1)) : end;
                  out.push_back(decode_value<W, Auto, T, EA>(src, beg, fin));
               }
            }
         }
         return out;
      }

      // Decode in-place: write fields directly into `out`. Used by
      // decode_vector to avoid the per-element `T tmp{}` + move-construct
      // when filling a pre-resized destination.
      //
      // The record frame is resolved by the shared span parser, which reads
      // the writer's fixed-region header: known trailing optional fields the
      // writer pruned decode as None, and unknown appended slots are skipped
      // while still delimiting the last known variable payload.
      template <std::size_t LegacyW, bool Auto, Record T, typename A, std::size_t... Is>
      void record_decode_into(std::span<const char> src, std::size_t pos,
                              std::size_t end, T& out,
                              std::index_sequence<Is...> seq)
      {
         constexpr std::size_t W = [] {
            if constexpr (Auto) return width_for<T, A>();
            else return LegacyW;
         }();
         enforce_exact_fixed_span<T>(src, pos, end);
         if constexpr (Auto)
         {
            if (pos > end || end > src.size()) size_failure("pssz: value span out of bounds");
            enforce_max_dynamic<T>(end - pos);
            enforce_fixed_caps<T>();
         }
         using R = record_traits<T>;
         constexpr auto layout = record_layout<W, T>(seq);
         std::array<::psio::detail::pssz_field_span, R::member_count> spans{};
         bool unknown = false;
         if (auto st = ::psio::detail::pssz_record_spans_inline<W>(
                src, pos, end, ::psio::is_dwnc_v<T>, layout, spans, unknown);
             !st.ok())
            status_failure(st);
         if (record_has_unpruned_tail<T>(src, spans, unknown, seq))
            size_failure("pssz: trailing absent optional field was not pruned");
         if constexpr (!is_fixed_v<T>)
            enforce_wire_span(end - pos - (::psio::is_dwnc_v<T> ? 0 : W));

         (
            ([&]
             {
                using F = typename R::template member_type<Is>;
                auto& fref = record_member<Is>(out);
                if (!spans[Is].present)
                {
                   // Only an omittable optional can be absent; the span
                   // parser rejects a missing required field.
                   if constexpr (omittable_field_v<T, Is>) fref.reset();
                   return;
                }
                using eff =
                   typename record_member_annotations<T, Is>::value_t;
                if constexpr (::psio::has_as_override_v<eff>)
                {
                   using Tag = ::psio::adapter_tag_of_t<eff>;
                   using Proj = ::psio::adapter<
                      std::remove_cvref_t<F>, Tag>;
                   fref = Proj::decode(std::span<const char>(
                      src.data() + spans[Is].begin,
                      spans[Is].end - spans[Is].begin));
                }
                else
                {
                   decode_into<W, Auto, F, field_ann<T, A, Is>>(src, spans[Is].begin,
                                                                spans[Is].end, fref);
                   // A member's declared annotations are part of its type
                   // (they selected the offset widths this frame was read
                   // with): the fail-fast decode refuses a member outside
                   // its length_bound / utf8 at any depth, as the validator
                   // and the encoder do.
                   if constexpr (ann::has_tree_specs_v<eff>)
                      if (auto st = ::psio::check_member_annotations(
                             fref, record_member_annotations<T, Is>::value,
                             "pssz: member length_bound violated", "pssz");
                          !st.ok())
                         status_failure(st);
                }
             }()),
            ...);
      }

      template <std::size_t W, bool Auto, Record T, typename A, std::size_t... Is>
      T record_decode(std::span<const char> src, std::size_t pos,
                      std::size_t end, std::index_sequence<Is...> seq)
      {
         T out{};
         record_decode_into<W, Auto, T, A>(src, pos, end, out, seq);
         return out;
      }

      template <std::size_t LegacyW, bool Auto, typename T, typename A>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end)
      {
         constexpr std::size_t W = [] {
            if constexpr (Auto) return width_for<T, A>();
            else return LegacyW;
         }();
         enforce_exact_fixed_span<T>(src, pos, end);
         if constexpr (Auto)
         {
            if (pos > end || end > src.size())
               size_failure("pssz: value span out of bounds");
            enforce_max_dynamic<T>(end - pos);
            enforce_fixed_caps<T>();
         }
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
            return T{decode_value<W, Auto, E, ann::cap_only<A>>(src, pos, end)};
         }
         else if constexpr (::psio::is_transparent_v<T>)
         {
            T out{};
            decode_into<W, Auto, ::psio::transparent_inner_t<T>, ann::cap_only<A>>(
               src, pos, end, ::psio::transparent_inner(out));
            return out;
         }
         else if constexpr (is_nested_v<T>)
            return T::unchecked(std::vector<char>(src.data() + pos, src.data() + end));
         else if constexpr (is_assoc_v<T>)
            return assoc_decode<W, Auto, T, A>(src, pos, end);
         else if constexpr (is_bounded_v<T>)
         {
            // The list, with this type's own table width.
            auto items = decode_vector<W, Auto, typename T::value_type, ann::element_ann<A>>(
               src, pos, end);
            if (items.size() > T::max_size_value)
               size_failure("pssz: bounded list exceeds its bound");
            return T{std::move(items)};
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
         else if constexpr (std::is_enum_v<T>)
         {
            // Any value of the underlying integer is accepted: the wire
            // carries the integer, and the enumerator set is not part of
            // the format (a newer writer may know enumerators this reader
            // does not). Callers that need a closed set check it themselves.
            std::underlying_type_t<T> raw{};
            std::memcpy(&raw, src.data() + pos, sizeof(raw));
            return static_cast<T>(raw);
         }
         else if constexpr (::psio::TimeValue<T>)
         {
            // Every tick count is a value.
            ::psio::time_rep_t<T> raw{};
            std::memcpy(&raw, src.data() + pos, sizeof(raw));
            return ::psio::time_from_ticks<T>(raw);
         }
         else if constexpr (std::is_arithmetic_v<T>)
            return decode_arith<W, T>(src, pos);
         else if constexpr (std::is_same_v<T, std::string>)
            return std::string(src.data() + pos, src.data() + end);
         else if constexpr (is_utf8_string_v<T>)
         {
            if (pos > end || end > src.size() || end - pos > T::max_size_value)
               size_failure("pssz: utf8_string exceeds its bound");
            if (auto st = ::psio::utf8_spec::validate(
                   std::span<const char>(src.data() + pos, end - pos));
                !st.ok())
               size_failure("pssz: utf8_string is not valid UTF-8");
            return T{std::string(src.data() + pos, src.data() + end)};
         }
         else if constexpr (is_std_array_v<T>)
            return decode_array<W, Auto, typename T::value_type,
                                std::tuple_size<T>::value, ann::element_ann<A>>(src, pos, end);
         else if constexpr (is_std_vector_v<T>)
            return decode_vector<W, Auto, typename T::value_type, ann::element_ann<A>>(
               src, pos, end);
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            if constexpr (!optional_uses_selector_v<V>)
            {
               // No selector: length alone disambiguates (empty vs not).
               if (pos >= end)
                  return std::optional<V>{};
               return std::optional<V>{decode_value<W, Auto, V, A>(src, pos, end)};
            }
            else
            {
               // 1-byte Union selector at pos: 0x00 = None, 0x01 = Some.
               // The selector is required, None carries nothing after it,
               // and no other selector value exists.
               if (pos >= end || end > src.size())
                  size_failure("pssz: optional selector missing");
               const std::uint8_t sel =
                  static_cast<unsigned char>(src[pos]);
               if (sel == 0)
               {
                  if (end - pos != 1)
                     size_failure("pssz: optional None has trailing bytes");
                  return std::optional<V>{};
               }
               if (sel != 1)
                  size_failure("pssz: optional selector not 0/1");
               return std::optional<V>{
                  decode_value<W, Auto, V, A>(src, pos + 1, end)};
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
                               decode_value<W, Auto,
                                  std::variant_alternative_t<Is, T>,
                                  ann::at_ann<A, Is>>(
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
            return record_decode<W, Auto, T, A>(
               src, pos, end, std::make_index_sequence<R::member_count>{});
         }
         else
         {
            PSIO_REJECT_MULTI_ASSOC("psio::pssz", T);
            static_assert(sizeof(T) == 0,
                          "psio::pssz: unsupported type in decode_value");
         }
      }

      // ── Structural validation ─────────────────────────────────────────────
      //
      // Full byte-level walker. Mirrors the SSZ shape but with W-byte
      // offsets and the u{W} fixed_region header on non-DWNC records.
      //
      // Node depth: the root is depth 0; every present record field,
      // sequence element, Some payload and variant payload is one deeper.
      // A transparent Box adds none. Nodes through kMaxValidationDepth are
      // accepted.
      //
      // `flags`, when supplied, carries two things through the walk:
      //   has_unknown    (out) set if any record in the frame carries optional
      //                  suffix slots this reader's layout does not know. Such
      //                  frames are structurally valid (compatible read);
      //                  exact-schema callers reject them.
      //   strict_strings (in)  strict mode: every plain string must be UTF-8.
      //                  By default a String is Vector(u8) carrying raw bytes
      //                  and passes through unchecked.
      //   exact          (in)  exact-schema mode (validate_strict): the bytes of
      //                  a nested value must themselves pass exact validation
      //                  as their type (nested.hpp).
      struct validate_flags
      {
         bool has_unknown    = false;
         bool strict_strings = false;
         bool exact          = false;
      };

      template <std::size_t W, bool Auto, typename T, typename A = no_ann>
      codec_status validate_value(std::span<const char> src, std::size_t pos,
                                  std::size_t end,
                                  std::size_t depth = 0,
                                  validate_flags* flags = nullptr) noexcept;
      template <std::size_t W, bool Auto, typename T, typename A = no_ann>
      codec_status validate_shape(std::span<const char> src, std::size_t pos,
                                  std::size_t end, std::size_t depth,
                                  validate_flags* flags) noexcept;

      // EA: the carrier of one element.
      template <std::size_t W, bool Auto, typename E, typename EA = no_ann>
      codec_status validate_vector_payload(std::span<const char> src,
                                           std::size_t            pos,
                                           std::size_t            end,
                                           std::size_t            depth,
                                           validate_flags* flags) noexcept
      {
         if (pos > end || end > src.size())
            return codec_fail("pssz: vector span out of bounds",
                              static_cast<std::uint32_t>(pos), "pssz");
         if (pos == end)
            return codec_ok();

         if constexpr (is_zero_fixed_v<E>)
            return codec_fail("pssz: nonempty zero-size vector is unsupported", 0, "pssz");
         else if constexpr (is_fixed_v<E>)
         {
            if constexpr (Auto && !fixed_caps_valid<E>())
               return codec_fail("pssz: fixed payload exceeds a declared maxDynamicData budget", 0, "pssz");
            const std::size_t esz = fixed_size_of<E>();
            if ((end - pos) % esz != 0)
               return codec_fail("pssz: vector span not a multiple of "
                                 "element size",
                                 static_cast<std::uint32_t>(pos), "pssz");
            if constexpr ((std::is_arithmetic_v<E> && !std::is_same_v<E, bool>) ||
                          (std::is_enum_v<E> && !::psio::has_invariant_v<E>))
            {
               // Leaf elements with no invalid bit patterns: the only check
               // a per-element walk would perform is the node depth.
               if (depth + 1 > kMaxValidationDepth)
                  return codec_fail("pssz: max depth exceeded",
                                    static_cast<std::uint32_t>(pos), "pssz");
            }
            else
            {
               const std::size_t n = (end - pos) / esz;
               for (std::size_t i = 0; i < n; ++i)
               {
                  auto st = validate_value<W, Auto, E>(src, pos + i * esz,
                                                  pos + (i + 1) * esz,
                                                  depth + 1, flags);
                  if (!st.ok()) return st;
               }
            }
            return codec_ok();
         }
         else
         {
            // Variable-element list with W-byte offset table.
            if (end - pos < W)
               return codec_fail("pssz: variable list missing offset table",
                                 static_cast<std::uint32_t>(pos), "pssz");
            if (!wire_span_fits(end - pos))
               return codec_fail("pssz: variable payload exceeds32-bit address space", 0, "pssz");
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            const std::uint32_t first = read_offset<W>(src, pos);
            // A nonempty list has at least one slot, so its first offset
            // (the table size) is a positive multiple of W.
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
               std::uint32_t stop  = (i + 1 < n)
                                        ? read_offset<W>(src, pos + (i + 1) * W)
                                        : span;
               auto st = validate_value<W, Auto, E, EA>(src, pos + off_i, pos + stop,
                                                   depth + 1, flags);
               if (!st.ok()) return st;
            }
            return codec_ok();
         }
      }

      // EA: the carrier of one element.
      template <std::size_t W, bool Auto, typename E, std::size_t N, typename EA = no_ann>
      codec_status validate_array_payload(std::span<const char> src,
                                          std::size_t            pos,
                                          std::size_t            end,
                                          std::size_t            depth,
                                          validate_flags* flags) noexcept
      {
         if (pos > end || end > src.size())
            return codec_fail("pssz: array span out of bounds",
                              static_cast<std::uint32_t>(pos), "pssz");
         if constexpr (N == 0)
            if (pos != end)
               return codec_fail("pssz: zero-count array payload must be empty", 0, "pssz");
         if constexpr (is_fixed_v<E>)
         {
            constexpr std::size_t esz = fixed_size_of<E>();
            if (end - pos != N * esz)
               return codec_fail("pssz: array span does not match its fixed size",
                                 static_cast<std::uint32_t>(pos), "pssz");
            if constexpr ((std::is_arithmetic_v<E> && !std::is_same_v<E, bool>) ||
                          (std::is_enum_v<E> && !::psio::has_invariant_v<E>))
            {
               if constexpr (N > 0)
                  if (depth + 1 > kMaxValidationDepth)
                     return codec_fail("pssz: max depth exceeded",
                                       static_cast<std::uint32_t>(pos), "pssz");
            }
            else
            {
               for (std::size_t i = 0; i < N; ++i)
               {
                  auto st = validate_value<W, Auto, E>(src, pos + i * esz,
                                                  pos + (i + 1) * esz,
                                                  depth + 1, flags);
                  if (!st.ok()) return st;
               }
            }
            return codec_ok();
         }
         else
         {
            if constexpr (N == 0)
               return codec_ok();
            if (end - pos < N * W)
               return codec_fail("pssz: array offset table truncated",
                                 static_cast<std::uint32_t>(pos), "pssz");
            if (!wire_span_fits(end - pos))
               return codec_fail("pssz: variable payload exceeds32-bit address space", 0, "pssz");
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
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
               std::uint32_t stop  = (i + 1 < N)
                                        ? read_offset<W>(src, pos + (i + 1) * W)
                                        : span;
               auto st = validate_value<W, Auto, E, EA>(src, pos + off_i, pos + stop,
                                                   depth + 1, flags);
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
         // The delimiter bit lives in the final byte, so that byte is never
         // zero in a canonical bitlist; trailing zero bytes are rejected.
         const std::size_t span = end - pos;
         const std::uint8_t lb = static_cast<std::uint8_t>(src[end - 1]);
         if (lb == 0)
            return codec_fail("pssz: bitlist missing delimiter",
                              static_cast<std::uint32_t>(pos), "pssz");
         int hi = 31 - __builtin_clz(static_cast<unsigned int>(lb));
         std::size_t bits =
            (span - 1) * 8 + static_cast<std::size_t>(hi);
         if (bits > MaxN)
            return codec_fail("pssz: bitlist bit_count exceeds bound",
                              static_cast<std::uint32_t>(pos), "pssz");
         return codec_ok();
      }

      // Length (bytes of a string, elements of a list) of an already
      // validated member payload, checked against the member's
      // length_bound. The bound looks through std::optional, as width
      // inference does (max_size.hpp); it constrains strings and lists
      // only, every other shape has no length to check.
      template <std::size_t LegacyW, bool Auto, typename F, typename A = no_ann>
      codec_status check_member_length_bound(
         std::span<const char> src, std::size_t pos, std::size_t end,
         const ::psio::length_bound& lb,
         const char* message = "pssz: member length_bound violated") noexcept
      {
         if constexpr (is_std_optional_v<F>)
         {
            using V = typename F::value_type;
            if constexpr (!optional_uses_selector_v<V>)
            {
               if (pos == end) return codec_ok();
               return check_member_length_bound<LegacyW, Auto, V, A>(src, pos, end, lb, message);
            }
            else
            {
               if (pos == end || src[pos] == 0) return codec_ok();
               return check_member_length_bound<LegacyW, Auto, V, A>(src, pos + 1, end, lb,
                                                                     message);
            }
         }
         else if constexpr (is_nested_v<F>)
         {
            // A nested member's length is its byte count.
            const std::size_t count = end - pos;
            if ((lb.max && count > *lb.max) || (lb.exact && count != *lb.exact) ||
                (lb.min && count < *lb.min))
               return codec_fail(message, static_cast<std::uint32_t>(pos), "pssz");
            return codec_ok();
         }
         else if constexpr (std::is_same_v<F, std::string> || is_utf8_string_v<F> ||
                            is_std_vector_v<F> || is_assoc_v<F> || is_bounded_v<F>)
         {
            std::size_t count = 0;
            if constexpr (std::is_same_v<F, std::string> || is_utf8_string_v<F>)
               count = end - pos;
            else
            {
               using E = typename F::value_type;
               if constexpr (is_zero_fixed_v<E>)
                  count = 0;
               else if constexpr (is_fixed_v<E>)
                  count = (end - pos) / fixed_size_of<E>();
               else
               {
                  // The list's own table width at this position.
                  constexpr std::size_t W = [] {
                     if constexpr (Auto) return width_for<F, A>();
                     else return LegacyW;
                  }();
                  count = pos == end ? 0 : read_offset<W>(src, pos) / W;
               }
            }
            if ((lb.max && count > *lb.max) || (lb.exact && count != *lb.exact) ||
                (lb.min && count < *lb.min))
               return codec_fail(message, static_cast<std::uint32_t>(pos), "pssz");
            return codec_ok();
         }
         else
            return codec_ok();
      }

      template <std::size_t W, bool Auto, Record T, typename A, std::size_t... Is>
      codec_status validate_record(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end,
                                   std::index_sequence<Is...> seq,
                                   std::size_t            depth,
                                   validate_flags* flags) noexcept
      {
         using R = record_traits<T>;
         // A record of plain numbers (every member an integer, a float or an
         // enum without an invariant: no byte pattern is invalid) written
         // in full is valid exactly when its frame has the type's size:
         // one compare instead of a walk over the fields. Every other frame
         // of such a type — short, long, with appended slots — takes the
         // general path below, so the outcomes are the same.
         if constexpr (((leaf_number_v<typename R::template member_type<Is>> &&
                         !::psio::has_as_override_v<
                            typename record_member_annotations<T, Is>::value_t>) &&
                        ... && (sizeof...(Is) > 0)))
         {
            constexpr std::size_t known = (fixed_size_of<typename R::template member_type<Is>>() +
                                           ... + std::size_t{0});
            constexpr std::size_t header = ::psio::is_dwnc_v<T> ? 0 : W;
            if (pos <= end && end <= src.size() && end - pos == header + known &&
                (header == 0 || read_offset<W>(src, pos) == known) &&
                depth + 1 <= kMaxValidationDepth)
               return codec_ok();
         }
         constexpr auto layout = record_layout<W, T>(seq);
         std::array<::psio::detail::pssz_field_span, R::member_count> spans{};
         bool unknown = false;
         // Resolves the frame from the writer's header: rejects a header
         // that exceeds the frame, cuts a known slot or omits a required
         // field; checks offsets are monotone, in bounds and start at the
         // fixed region; accepts pruned trailing optionals and unknown
         // appended optional slots.
         auto status = ::psio::detail::pssz_record_spans_inline<W>(
            src, pos, end, ::psio::is_dwnc_v<T>, layout, spans, unknown);
         if (!status.ok()) return status;
         if constexpr (!is_fixed_v<T>)
            if (!wire_span_fits(end - pos - (::psio::is_dwnc_v<T> ? 0 : W)))
               return codec_fail("pssz: variable payload exceeds32-bit address space", 0, "pssz");
         if (record_has_unpruned_tail<T>(src, spans, unknown, seq))
            return codec_fail("pssz: trailing absent optional field was not pruned",
                              static_cast<std::uint32_t>(pos), "pssz");
         if (flags) flags->has_unknown |= unknown;
         (
            ([&]
             {
                if (!status.ok() || !spans[Is].present) return;
                using F = typename R::template member_type<Is>;
                using eff =
                   typename record_member_annotations<T, Is>::value_t;
                if constexpr (::psio::has_as_override_v<eff>)
                {
                   if (depth >= kMaxValidationDepth)
                      status = codec_fail("pssz: max depth exceeded", 0, "pssz");
                   else
                   {
                      using Proj = ::psio::adapter<
                         std::remove_cvref_t<F>, ::psio::adapter_tag_of_t<eff>>;
                      status = Proj::validate(std::span<const char>(
                         src.data() + spans[Is].begin,
                         spans[Is].end - spans[Is].begin));
                   }
                }
                else
                {
                   // The member's declared annotations are part of its
                   // type: validate_value checks the length_bound / utf8 of
                   // its carrier at every position it reaches.
                   status = validate_value<W, Auto, F, field_ann<T, A, Is>>(
                      src, spans[Is].begin, spans[Is].end, depth + 1,
                      flags);
                }
             }()),
            ...);
         return status;
      }

      // Every value is validated through here: the structural walk of
      // validate_shape, then — for a type that has one, and only then — its
      // invariant (invariant.hpp) on the value materialised from exactly
      // this span. The nested values were validated the same way by the
      // walk, so invariants run innermost first, for every shape the walk
      // knows.
      // What the carrier states for this position itself, on a span the
      // structural walk has accepted: its length_bound (bytes of a string,
      // elements of a list) and utf8. A std::optional is looked through: its
      // payload is checked when the walk reaches it with the same carrier.
      template <std::size_t LegacyW, bool Auto, typename T, typename A>
      codec_status check_position(std::span<const char> src, std::size_t pos,
                                  std::size_t end) noexcept
      {
         if constexpr (!std::is_same_v<A, no_ann> && !is_std_optional_v<T>)
         {
            if constexpr (ann::has_length_bound_v<A>)
               if (auto st = check_member_length_bound<LegacyW, Auto, T, A>(
                      src, pos, end, std::get<::psio::length_bound>(A::value));
                   !st.ok())
                  return st;
            if constexpr (ann::has_utf8_v<A> && std::is_same_v<T, std::string>)
               return ::psio::utf8_spec::validate(
                  std::span<const char>(src.data() + pos, end - pos));
         }
         return codec_ok();
      }

      template <std::size_t LegacyW, bool Auto, typename T, typename A>
      codec_status validate_value(std::span<const char> src, std::size_t pos,
                                  std::size_t end,
                                  std::size_t depth,
                                  validate_flags* flags) noexcept
      {
         // The common case — no annotation at this position, no invariant —
         // is the structural walk and nothing after it.
         if constexpr (std::is_same_v<A, no_ann> && !::psio::has_invariant_v<T>)
            return validate_shape<LegacyW, Auto, T, A>(src, pos, end, depth, flags);
         else
         {
            if (auto st = validate_shape<LegacyW, Auto, T, A>(src, pos, end, depth, flags);
                !st.ok())
               return st;
            if constexpr (!std::is_same_v<A, no_ann>)
               if (auto st = check_position<LegacyW, Auto, T, A>(src, pos, end); !st.ok())
                  return st;
            if constexpr (::psio::has_invariant_v<T>)
               return ::psio::check_invariant(decode_value<LegacyW, Auto, T, A>(src, pos, end));
            else
               return codec_ok();
         }
      }

      template <std::size_t LegacyW, bool Auto, typename T, typename A>
      codec_status validate_shape(std::span<const char> src, std::size_t pos,
                                  std::size_t end,
                                  std::size_t depth,
                                  validate_flags* flags) noexcept
      {
         constexpr std::size_t W = [] {
            if constexpr (Auto) return width_for<T, A>();
            else return LegacyW;
         }();
         if (pos > end || end > src.size())
            return codec_fail("pssz: value span out of bounds", 0, "pssz");
         if (auto st = check_zero_fixed_span<T>(src, pos, end); !st.ok())
            return st;
         if constexpr (Auto)
         {
            if (auto cap = check_max_dynamic<T>(src.subspan(pos, end - pos)); !cap.ok())
               return cap;
            if constexpr (!fixed_caps_valid<T>())
               return codec_fail("pssz: fixed payload exceeds a declared maxDynamicData budget", 0, "pssz");
         }
         if (depth > kMaxValidationDepth)
            return codec_fail("pssz: max depth exceeded",
                              static_cast<std::uint32_t>(pos), "pssz");
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::pssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            return Proj::validate(std::span<const char>(
               src.data() + pos, end - pos));
         }
         else if constexpr (is_psio_box_v<T>)
            return validate_value<W, Auto, typename is_psio_box<T>::elem, ann::cap_only<A>>(
               src, pos, end, depth, flags);
         else if constexpr (::psio::is_transparent_v<T>)
            return validate_value<W, Auto, ::psio::transparent_inner_t<T>, ann::cap_only<A>>(
               src, pos, end, depth, flags);
         else if constexpr (is_nested_v<T>)
            return ::psio::detail::nested_impl::pssz_validate<T>(
               src.subspan(pos, end - pos), depth, flags);
         else if constexpr (is_assoc_v<T>)
            return assoc_validate<W, Auto, T, A>(src, pos, end, depth, flags);
         else if constexpr (is_bounded_v<T>)
         {
            // The list, with this type's own table width.
            if (auto st = validate_vector_payload<W, Auto, typename T::value_type,
                                                  ann::element_ann<A>>(src, pos, end, depth, flags);
                !st.ok())
               return st;
            return check_member_length_bound<W, Auto, T, A>(
               src, pos, end,
               ::psio::length_bound{.max = static_cast<std::uint32_t>(T::max_size_value)},
               "pssz: bounded list exceeds its bound");
         }
         else if constexpr (is_fixed_v<T> && !Record<T>)
         {
            if (end - pos != fixed_size_of<T>())
               return codec_fail("pssz: fixed primitive span mismatch",
                                 static_cast<std::uint32_t>(pos), "pssz");
            // An enum is its underlying integer: every bit pattern of the
            // right size is valid (no enumerator-set check).
            if constexpr (std::is_same_v<T, bool>)
            {
               if (static_cast<unsigned char>(src[pos]) > 1)
                  return codec_fail("pssz: bool is not 0/1",
                                    static_cast<std::uint32_t>(pos), "pssz");
            }
            else if constexpr (is_std_array_v<T>)
               return validate_array_payload<W, Auto, typename T::value_type,
                                             std::tuple_size_v<T>>(
                  src, pos, end, depth, flags);
            else if constexpr (is_bitvector_v<T>)
            {
               if constexpr (T::size_value % 8 != 0)
                  if ((static_cast<unsigned char>(src[end - 1]) >>
                       (T::size_value % 8)) != 0)
                     return codec_fail("pssz: nonzero unused bitvector bits",
                                       static_cast<std::uint32_t>(pos), "pssz");
            }
            return codec_ok();
         }
         else if constexpr (is_bitlist_v<T>)
            return validate_bitlist_payload<W, T::max_size_value>(src, pos, end);
         else if constexpr (is_utf8_string_v<T>)
         {
            if (end - pos > T::max_size_value)
               return codec_fail("pssz: utf8_string exceeds its bound",
                                 static_cast<std::uint32_t>(pos), "pssz");
            return ::psio::utf8_spec::validate(
               std::span<const char>(src.data() + pos, end - pos));
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            // A plain std::string is Vector(u8): raw bytes pass through.
            // UTF-8 is checked only in strict mode, by the utf8_spec
            // annotation, and by the utf8_string<N> wrapper.
            if (flags && flags->strict_strings)
               return ::psio::utf8_spec::validate(
                  std::span<const char>(src.data() + pos, end - pos));
            return codec_ok();
         }
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            return validate_array_payload<W, Auto, E, N, ann::element_ann<A>>(
               src, pos, end, depth, flags);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            return validate_vector_payload<W, Auto, E, ann::element_ann<A>>(
               src, pos, end, depth, flags);
         }
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            if constexpr (!optional_uses_selector_v<V>)
            {
               // Positive-minimum V: an empty span is None, anything else
               // is the bare encoding of V (exactly fixed_size_of<V> bytes
               // when V is fixed).
               const std::size_t span = end - pos;
               if (span == 0)
                  return codec_ok();
               if constexpr (is_fixed_v<V>)
                  if (span != fixed_size_of<V>())
                     return codec_fail("pssz: optional<fixed> span mismatch",
                                       static_cast<std::uint32_t>(pos), "pssz");
               return validate_value<W, Auto, V, A>(src, pos, end, depth + 1,
                                               flags);
            }
            else
            {
               // V can encode to zero bytes: 1-byte selector + payload.
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
               return validate_value<W, Auto, V, A>(src, pos + 1, end, depth + 1,
                                               flags);
            }
         }
         else if constexpr (is_std_variant_v<T>)
         {
            if (pos >= end)
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
               const bool dispatched = (((sel == Js)
                    ? (err = validate_value<W, Auto,
                                std::variant_alternative_t<Js, T>,
                                ann::at_ann<A, Js>>(
                          src, pos + 1, end, depth + 1, flags),
                       true)
                    : false) ||
                ...);
               (void)dispatched;
               return err;
            }(std::make_index_sequence<NA>{});
         }
         else if constexpr (Record<T>)
         {
            using R = record_traits<T>;
            return validate_record<W, Auto, T, A>(
               src, pos, end, std::make_index_sequence<R::member_count>{},
               depth, flags);
         }
         else
         {
            PSIO_REJECT_MULTI_ASSOC("psio::pssz", T);
            return codec_fail("pssz: unsupported type in validate",
                              static_cast<std::uint32_t>(pos), "pssz");
         }
      }

      // ── Throwing validation (native) / aborting (exception-disabled) ──────
      //
      // One walker defines the accepted set: the fail-fast entry point runs
      // the status validator and raises its error, so `validate` and
      // `validate_or_throw` cannot disagree about a frame.

      [[noreturn, gnu::cold]] inline void
      pssz_throw_fail(std::string_view msg, std::uint32_t off)
      {
#if defined(__cpp_exceptions) && defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
         throw codec_exception{codec_error{msg, off, "pssz"}};
#else
         (void)msg;
         (void)off;
         std::abort();
#endif
      }

      template <std::size_t W, bool Auto, typename T>
      void validate_or_throw_value(std::span<const char> src,
                                   std::size_t pos, std::size_t end)
      {
         if (auto st = validate_value<W, Auto, T>(src, pos, end); !st.ok())
            [[unlikely]] status_failure(st);
      }

      // ── Exact-schema (strict) validation ──────────────────────────────────
      //
      // Structural validation that additionally rejects unknown optional
      // suffix fields. Native builds then materialize the value to run its
      // semantic specs and to require the canonical encoding (which rejects
      // an explicitly stored trailing None the encoder would have pruned).
      //
      // Exception-disabled guests never materialize: a wrapper or adapter may
      // abort on construction, which a guest cannot catch. They report an
      // explicit unsupported status for types that need semantic work and
      // structural success otherwise (docs/guest-codec-preparation.md).
      // `depth`: the node depth of the value's root (nonzero for the bytes
      // of a nested value, which are checked by this same function).
      template <std::size_t W, bool Auto, typename T>
      codec_status validate_exact_value(std::span<const char> bytes,
                                        std::size_t depth = 0) noexcept
      {
         if (auto cap = check_max_dynamic<T>(bytes); !cap.ok())
            return cap;
         validate_flags flags{.has_unknown = false, .strict_strings = true, .exact = true};
         auto st = validate_value<W, Auto, T>(bytes, 0, bytes.size(), depth, &flags);
         if (!st.ok())
            return st;
         if (flags.has_unknown)
            return codec_fail("pssz: unknown fields in exact validation", 0, "pssz");
#if defined(__cpp_exceptions)
         try
         {
            T decoded = decode_value<W, Auto, T>(bytes, 0, bytes.size());
            if constexpr (::psio::Reflected<T>)
            {
               auto specs = ::psio::validate_specs_on_value(decoded);
               if (!specs.ok())
                  return specs;
            }
            const std::size_t size = size_of_v<W, Auto>(decoded);
            if (size != bytes.size())
               return codec_fail("pssz: noncanonical encoding", 0, "pssz");
            std::vector<char> canonical(size);
            ::psio::fast_buf_stream sink(canonical.data(), canonical.size());
            encode_value<W, Auto>(decoded, sink);
            if (!std::equal(canonical.begin(), canonical.end(), bytes.begin()))
               return codec_fail("pssz: noncanonical encoding", 0, "pssz");
            return st;
         }
         catch (...)
         {
            return codec_fail(
               "pssz: decode failed during validate_strict", 0,
               "pssz");
         }
#else
         if constexpr (!::psio::detail::vstrict::can_validate_without_materialization<T>())
            return codec_fail("pssz: guest strict semantic validation unsupported", 0, "pssz");
         return st;
#endif
      }

   }  // namespace detail::pssz_impl

   // ── Format tags ───────────────────────────────────────────────────────────

   // ── Width selection ───────────────────────────────────────────────────
   //
   // pssz is one format tag. Its internal offset width (1, 2, or 4
   // bytes) is selected at compile time from the explicit type-level
   // `max_dynamic_data_v<T>` budget, when present. Without that promise,
   // `max_encoded_size<T>()` provides a conservative bound inferred from the
   // fields' annotations, including full declared child budgets, with each
   // type's own header / slots / offset table counted at that type's own
   // width (max_size.hpp `fit`). A declared budget takes precedence in either
   // direction. A list (std::vector, psio::bounded, a map, a set) follows
   // the same rule for its offset table: 4 when unbounded, else the width
   // class of its own bound.
   //
   //   budget/bound ≤ 0xff   → W = 1
   //   budget/bound ≤ 0xffff → W = 2
   //   else / unbounded → W = 4
   //
   // This is a per-logical-type choice, independent of the enclosing type and
   // of a particular value's payload size. The explicit pssz_<W> compatibility
   // profile instead retains its original single W throughout recursion.

   namespace detail::pssz_impl
   {
      // A reflected record or an annotated tuple (a method's argument
      // tuple): a type whose width is its own wherever it appears. A plain
      // tuple / pair is not one: it has no name and takes the width of its
      // position.
      template <typename T>
      inline constexpr bool record_boundary_v =
         Record<T> && (!is_std_tuple_v<T> || requires { typename T::psio_annotated_tuple; });

      // The offset width of T at a position carrying A's annotations: of a
      // record's or tuple's header and member slots, of the offset table of
      // a list (std::vector, psio::bounded, a map, a set) or array of
      // variable elements. It is the width class of T's declared
      // maxDynamicData when it has one, else of the bound inferred for it
      // with the annotations of its position (max_size.hpp) — exactly the
      // bound the equivalent wrapper types would give. An unbounded type
      // (a plain std::vector, a record holding one) is 4 wide.
      template <typename T, typename A>
      consteval std::size_t width_for() noexcept
      {
         constexpr auto m = [] {
            std::optional<std::size_t> own;
            if constexpr (::psio::max_dynamic_data_v<T>.has_value())
               own = ::psio::max_dynamic_data_v<T>;
            else
               own = ::psio::detail::max_size_impl::max_field_with_annots<T>(A::value);
            // The cap of the enclosing record bounds everything beneath it
            // (docs/pssz-spec.md 3.6). A record keeps its own width
            // wherever it appears: the cap does not cross into one.
            if constexpr (!record_boundary_v<T>)
               if (A::cap != ann::no_cap && (!own || *own > A::cap))
                  own = A::cap;
            return own;
         }();
         if constexpr (m.has_value())
            return *m <= 0xFFu ? 1 : *m <= 0xFFFFu ? 2 : 4;
         else
            return 4;
      }
   }  // namespace detail::pssz_impl

   namespace detail::pssz_impl
   {
      // Whether every value nested in a T at a position carrying A has the
      // offset width its type alone gives it. A typed view (pssz_view.hpp)
      // re-selects widths from types, so it can address exactly such members.
      template <typename T, typename A>
      consteval bool widths_unchanged() noexcept
      {
         // Only a type that writes offsets has a width to compare: a list,
         // array, map or set of variable elements and a tuple / pair (a
         // reflected record's width is its own).
         if constexpr (std::is_same_v<A, no_ann>)
            return true;
         else if constexpr (is_psio_box_v<T>)
            return widths_unchanged<typename is_psio_box<T>::elem, ann::cap_only<A>>();
         else if constexpr (::psio::is_transparent_v<T>)
            return widths_unchanged<::psio::transparent_inner_t<T>, ann::cap_only<A>>();
         else if constexpr (is_std_optional_v<T>)
            return widths_unchanged<typename T::value_type, A>();
         else if constexpr (is_std_vector_v<T> || is_bounded_v<T> || is_std_array_v<T>)
            return (is_fixed_v<typename T::value_type> ||
                    width_for<T, A>() == width_for<T, no_ann>()) &&
                   widths_unchanged<typename T::value_type, ann::element_ann<A>>();
         else if constexpr (is_assoc_v<T>)
            return (is_fixed_v<::psio::detail::assoc_entry_t<T>> ||
                    width_for<T, A>() == width_for<T, no_ann>()) &&
                   widths_unchanged<::psio::detail::assoc_entry_t<T>, ann::entry_ann<T, A>>();
         else if constexpr (is_std_variant_v<T>)
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (widths_unchanged<std::variant_alternative_t<Is, T>, ann::at_ann<A, Is>>() &&
                       ...);
            }(std::make_index_sequence<std::variant_size_v<T>>{});
         else if constexpr (is_std_tuple_v<T> && !requires { typename T::psio_annotated_tuple; })
            return width_for<T, A>() == width_for<T, no_ann>() &&
                   []<std::size_t... Is>(std::index_sequence<Is...>) {
                      return (widths_unchanged<std::remove_const_t<std::tuple_element_t<Is, T>>,
                                               ann::at_ann<A, Is>>() &&
                              ...);
                   }(std::make_index_sequence<std::tuple_size_v<T>>{});
         else
            return true;
      }
   }  // namespace detail::pssz_impl

   template <typename T>
   consteval std::size_t auto_pssz_width() noexcept
   {
      // A declared budget is the lifetime wire-width promise for this logical
      // type. It stays authoritative even while current fields need less space.
      constexpr auto m = [] {
         if constexpr (::psio::max_dynamic_data_v<T>.has_value())
            return ::psio::max_dynamic_data_v<T>;
         else return ::psio::max_encoded_size<T>();
      }();
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

   // ── A capped record holding a wider record ────────────────────────────
   //
   // The cap of a record bounds the lists, maps, tuples, ... beneath it, and
   // they take their offset width from it (docs/pssz-spec.md 3.6). A record
   // beneath it does not: a record's width is its own wherever it appears.
   // So a child record that is wider than its capped parent — an unbounded
   // Child (4-byte offsets) inside a parent capped at 255 — spends offset
   // bytes on a capacity it can never use there.
   //
   // `pssz_child_wider_than_cap_v<T>` is true when T has an explicit
   // maxDynamicData and a record (or method argument tuple) wider than the
   // cap's width class lies beneath it, with no other record in between.
   // The remedy is a maxDynamicData (or member bounds) on the child. It is a
   // query, not an error: such a type is valid, and a schema can check it
   // with
   //
   //   static_assert(!psio::pssz_child_wider_than_cap_v<Transfer>);
   namespace detail::pssz_impl
   {
      template <typename T>
      consteval bool holds_wider_record(std::size_t width) noexcept
      {
         if constexpr (is_psio_box_v<T>)
            return holds_wider_record<typename is_psio_box<T>::elem>(width);
         else if constexpr (::psio::format_should_dispatch_adapter_v<::psio::pssz, T> ||
                            is_nested_v<T>)
            return false;
         else if constexpr (::psio::is_transparent_v<T>)
            return holds_wider_record<::psio::transparent_inner_t<T>>(width);
         else if constexpr (is_assoc_v<T>)
            return holds_wider_record<::psio::detail::assoc_entry_t<T>>(width);
         else if constexpr (is_std_optional_v<T> || is_std_vector_v<T> || is_bounded_v<T> ||
                            is_std_array_v<T>)
            return holds_wider_record<typename T::value_type>(width);
         else if constexpr (is_std_variant_v<T>)
            return []<std::size_t... Is>(std::size_t w, std::index_sequence<Is...>) {
               return (holds_wider_record<std::variant_alternative_t<Is, T>>(w) || ...);
            }(width, std::make_index_sequence<std::variant_size_v<T>>{});
         else if constexpr (record_boundary_v<T>)
            return ::psio::auto_pssz_width<T>() > width;
         else if constexpr (is_std_tuple_v<T>)
            return []<std::size_t... Is>(std::size_t w, std::index_sequence<Is...>) {
               return (holds_wider_record<std::remove_const_t<std::tuple_element_t<Is, T>>>(w) ||
                       ...);
            }(width, std::make_index_sequence<std::tuple_size_v<T>>{});
         else
            return false;
      }
      template <typename T>
      consteval bool child_wider_than_cap() noexcept
      {
         if constexpr (!record_boundary_v<T> || has_binary_adapter_v<T>)
            return false;
         else if constexpr (!::psio::max_dynamic_data_v<T>.has_value())
            return false;
         else
            return []<std::size_t... Is>(std::index_sequence<Is...>) {
               const std::size_t width = ::psio::auto_pssz_width<T>();
               return ((!::psio::has_as_override_v<
                           typename record_member_annotations<T, Is>::value_t> &&
                        holds_wider_record<std::remove_cvref_t<
                           typename record_traits<T>::template member_type<Is>>>(width)) ||
                       ...);
            }(std::make_index_sequence<record_traits<T>::member_count>{});
      }
   }  // namespace detail::pssz_impl
   template <typename T>
   inline constexpr bool pssz_child_wider_than_cap_v =
      detail::pssz_impl::child_wider_than_cap<T>();

   // ── Evolution check ───────────────────────────────────────────────────
   //
   // `pssz_evolves_from<Old, New>` is true iff data written for `Old` is
   // readable as `New` and data written for `New` is readable as `Old` under
   // the automatic (type-derived width) profile:
   //
   //   - New's fields are Old's fields, in order, followed only by plain
   //     optional fields (an appended field must own an offset slot and be
   //     able to be absent);
   //   - the two have the same offset width. An optional field may not be
   //     added if it would change the type's width: an explicit
   //     maxDynamicData always rules, and without one the width falls back to
   //     the bound inferred from the fields, which an appended field can
   //     move. Reserve room with an explicit maxDynamicData to evolve;
   //   - DWNC status is unchanged, and a DWNC record does not gain fields.
   //
   // A prefix field may itself have evolved (a nested record, or a vector /
   // array / optional / variant of one) provided it meets the same rules and
   // keeps its own width and presence form. The wire carries no schema, so
   // this is a compile-time schema-upgrade check, e.g.
   //
   //   static_assert(psio::pssz_evolves_from<AccountV1, AccountV2>);
   namespace detail::pssz_impl
   {
      template <typename Old, typename New>
      consteval bool layout_evolves() noexcept;

      template <typename T, typename A>
      consteval bool widths_unchanged() noexcept;

      // Field I of Old and of New agree on what their annotations do to the
      // widths of the values nested in the field: either neither narrows
      // anything (the widths are those of the types, which layout_evolves
      // compares), or the two fields are the same type with the same
      // annotations.
      template <Record Old, Record New, std::size_t I>
      consteval bool member_widths_agree() noexcept
      {
         using FO = typename record_traits<Old>::template member_type<I>;
         using FN = typename record_traits<New>::template member_type<I>;
         if constexpr (widths_unchanged<FO, member_ann<Old, I>>() &&
                       widths_unchanged<FN, member_ann<New, I>>())
            return true;
         else if constexpr (std::is_same_v<FO, FN> &&
                            std::is_same_v<typename record_member_annotations<Old, I>::value_t,
                                           typename record_member_annotations<New, I>::value_t>)
            return record_member_annotations<Old, I>::value ==
                   record_member_annotations<New, I>::value;
         else
            return false;
      }

      template <Record Old, Record New>
      consteval bool record_evolves() noexcept
      {
         using RO = record_traits<Old>;
         using RN = record_traits<New>;
         constexpr std::size_t NO = RO::member_count;
         constexpr std::size_t NN = RN::member_count;
         if constexpr (is_std_tuple_v<Old> != is_std_tuple_v<New> ||
                       ::psio::is_dwnc_v<Old> != ::psio::is_dwnc_v<New> ||
                       has_binary_adapter_v<Old> || has_binary_adapter_v<New> || NN < NO)
            return false;
         else if constexpr (::psio::is_dwnc_v<Old> && NN != NO)
            return false;
         else
         {
            constexpr bool prefix = []<std::size_t... Is>(std::index_sequence<Is...>) {
               return ((::psio::has_as_override_v<
                           typename record_member_annotations<Old, Is>::value_t> ==
                           ::psio::has_as_override_v<
                              typename record_member_annotations<New, Is>::value_t> &&
                        (::psio::has_as_override_v<
                            typename record_member_annotations<Old, Is>::value_t>
                            ? std::is_same_v<typename RO::template member_type<Is>,
                                             typename RN::template member_type<Is>>
                            : (layout_evolves<typename RO::template member_type<Is>,
                                              typename RN::template member_type<Is>>() &&
                               member_widths_agree<Old, New, Is>()))) &&
                       ...);
            }(std::make_index_sequence<NO>{});
            constexpr bool suffix = []<std::size_t... Is>(std::index_sequence<Is...>) {
               return (omittable_field_v<New, NO + Is> && ...);
            }(std::make_index_sequence<NN - NO>{});
            return prefix && suffix;
         }
      }

      template <typename Old, typename New>
      consteval bool layout_evolves() noexcept
      {
         if constexpr (std::is_same_v<Old, New>)
            return true;
         else if constexpr (::psio::is_transparent_v<Old> || ::psio::is_transparent_v<New>)
         {
            // A transparent type keeps its identity: it evolves only from a
            // transparent type of the same name, as far as the members do.
            if constexpr (::psio::is_transparent_v<Old> && ::psio::is_transparent_v<New>)
               return ::psio::reflect<Old>::name == ::psio::reflect<New>::name &&
                      layout_evolves<::psio::transparent_inner_t<Old>,
                                     ::psio::transparent_inner_t<New>>();
            else
               return false;
         }
         else if constexpr (::psio::auto_pssz_width<Old>() != ::psio::auto_pssz_width<New>() ||
                            is_fixed_v<Old> != is_fixed_v<New>)
            return false;
         else if constexpr (is_psio_box_v<Old> && is_psio_box_v<New>)
            return layout_evolves<typename is_psio_box<Old>::elem,
                                  typename is_psio_box<New>::elem>();
         else if constexpr (is_nested_v<Old> && is_nested_v<New>)
            return layout_evolves<typename is_nested<Old>::inner,
                                  typename is_nested<New>::inner>();
         else if constexpr (is_assoc_v<Old> || is_assoc_v<New>)
            return assoc_evolves<Old, New>();
         else if constexpr (is_std_optional_v<Old> && is_std_optional_v<New>)
            return optional_uses_selector_v<typename Old::value_type> ==
                      optional_uses_selector_v<typename New::value_type> &&
                   layout_evolves<typename Old::value_type, typename New::value_type>();
         else if constexpr (is_std_vector_v<Old> && is_std_vector_v<New>)
            return layout_evolves<typename Old::value_type, typename New::value_type>();
         else if constexpr (is_std_array_v<Old> && is_std_array_v<New>)
         {
            if constexpr (std::tuple_size_v<Old> != std::tuple_size_v<New>)
               return false;
            else
               return layout_evolves<typename Old::value_type, typename New::value_type>();
         }
         else if constexpr (is_std_variant_v<Old> && is_std_variant_v<New>)
         {
            if constexpr (std::variant_size_v<Old> != std::variant_size_v<New>)
               return false;
            else
               return []<std::size_t... Is>(std::index_sequence<Is...>) {
                  return (layout_evolves<std::variant_alternative_t<Is, Old>,
                                         std::variant_alternative_t<Is, New>>() &&
                          ...);
               }(std::make_index_sequence<std::variant_size_v<Old>>{});
         }
         else if constexpr (Record<Old> && Record<New>)
            return record_evolves<Old, New>();
         else
            return false;
      }
   }  // namespace detail::pssz_impl

   template <typename Old, typename New>
   inline constexpr bool pssz_evolves_from_v =
      detail::pssz_impl::layout_evolves<Old, New>();

   template <typename Old, typename New>
   concept pssz_evolves_from = pssz_evolves_from_v<Old, New>;

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
         detail::pssz_impl::enforce_encodable(v);
         const std::size_t total = detail::pssz_impl::size_of_v<W>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         const std::size_t orig = sink.size();
         sink.resize(detail::pssz_impl::checked_add(orig, total));
         ::psio::fast_buf_stream fbs(sink.data() + orig, total);
         detail::pssz_impl::encode_value<W>(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          pssz_<W>, const T& v)
      {
         detail::pssz_impl::enforce_encodable(v);
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
         return detail::pssz_impl::decode_value<W, false, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of),
                                    pssz_<W>, const T& v)
      {
         return detail::pssz_impl::size_of_v<W>(v);
      }

      // Compatible structural validation: frames carrying unknown optional
      // suffix fields are accepted (use validate_pssz_compatible to learn
      // whether any were present, validate_strict to reject them).
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     pssz_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = detail::pssz_impl::check_max_dynamic<T>(bytes);
             !st.ok())
            return st;
         return detail::pssz_impl::validate_value<W, false, T>(bytes, 0,
                                                        bytes.size());
      }

      template <typename T>
      friend void tag_invoke(decltype(::psio::validate_or_throw<T>),
                             pssz_<W>, T*,
                             std::span<const char> bytes)
      {
         detail::pssz_impl::enforce_max_dynamic<T>(bytes.size());
         detail::pssz_impl::validate_or_throw_value<W, false, T>(
            bytes, 0, bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     pssz_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::pssz_impl::validate_exact_value<W, false, T>(bytes);
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           pssz_<W>, T*,
                                           std::span<const char> bytes) noexcept
      {
         return std::make_unique<T>(
            detail::pssz_impl::decode_value<W, false, T>(bytes, 0, bytes.size()));
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
         detail::pssz_impl::enforce_encodable(v);
         const std::size_t total = detail::pssz_impl::size_of_v<W, true>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         const std::size_t orig = sink.size();
         sink.resize(detail::pssz_impl::checked_add(orig, total));
         ::psio::fast_buf_stream fbs(sink.data() + orig, total);
         detail::pssz_impl::encode_value<W, true>(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          pssz, const T& v)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         detail::pssz_impl::enforce_encodable(v);
         const std::size_t total = detail::pssz_impl::size_of_v<W, true>(v);
         detail::pssz_impl::enforce_max_dynamic<T>(total);
         std::vector<char> out(total);
         ::psio::fast_buf_stream fbs(out.data(), total);
         detail::pssz_impl::encode_value<W, true>(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), pssz, T*,
                          std::span<const char> bytes)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return detail::pssz_impl::decode_value<W, true, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of),
                                    pssz, const T& v)
      {
         return detail::pssz_impl::size_of_v<auto_pssz_width_v<T>, true>(v);
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
         return detail::pssz_impl::validate_value<W, true, T>(bytes, 0,
                                                        bytes.size());
      }

      template <typename T>
      friend void tag_invoke(decltype(::psio::validate_or_throw<T>),
                             pssz, T*, std::span<const char> bytes)
      {
         detail::pssz_impl::enforce_max_dynamic<T>(bytes.size());
         constexpr std::size_t W = auto_pssz_width_v<T>;
         detail::pssz_impl::validate_or_throw_value<W, true, T>(
            bytes, 0, bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     pssz, T*,
                                     std::span<const char> bytes) noexcept
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return detail::pssz_impl::validate_exact_value<W, true, T>(bytes);
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           pssz, T*,
                                           std::span<const char> bytes) noexcept
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return std::make_unique<T>(
            detail::pssz_impl::decode_value<W, true, T>(bytes, 0, bytes.size()));
      }
   };

   PSIO_FORMAT_HELPERS(pssz)

   // ── Checked compatible reads ──────────────────────────────────────────
   //
   // Validate the whole known frame, accept unknown optional suffix fields
   // and report whether any were present (including inside nested records
   // and containers). A caller that intends to rewrite the value must
   // reject `has_unknown`: re-encoding the older typed value drops them.

   template <typename T, std::size_t W>
   [[nodiscard]] codec_status validate_pssz_compatible(
      pssz_<W>, std::span<const char> bytes, bool& has_unknown) noexcept
   {
      has_unknown = false;
      auto status = detail::pssz_impl::check_max_dynamic<T>(bytes);
      if (!status.ok()) return status;
      detail::pssz_impl::validate_flags flags;
      status = detail::pssz_impl::validate_value<W, false, T>(
         bytes, 0, bytes.size(), 0, &flags);
      if (status.ok()) has_unknown = flags.has_unknown;
      return status;
   }

   // Automatic profile: every logical type keeps its own width, exactly as
   // the automatic encoder and validator do.
   template <typename T>
   [[nodiscard]] codec_status validate_pssz_compatible(
      pssz, std::span<const char> bytes, bool& has_unknown) noexcept
   {
      has_unknown = false;
      auto status = detail::pssz_impl::check_max_dynamic<T>(bytes);
      if (!status.ok()) return status;
      detail::pssz_impl::validate_flags flags;
      status = detail::pssz_impl::validate_value<auto_pssz_width_v<T>, true, T>(
         bytes, 0, bytes.size(), 0, &flags);
      if (status.ok()) has_unknown = flags.has_unknown;
      return status;
   }

   namespace detail::pssz_impl
   {
      // Test support: the frame an encoder that does not check member
      // length_bounds or bitlist capacity would write. Everything else
      // (depth, UTF-8 wrappers, caps, offset widths) fails as in encode.
      // Readers must reject such frames; production code uses encode.
      template <typename Fmt, typename T>
      std::vector<char> encode_ignoring_length_bounds(Fmt, const T& value)
      {
         if (auto status = check_encode_depth_impl(value, 0, false); !status.ok())
            status_failure(status);
         constexpr bool Auto = std::is_same_v<Fmt, ::psio::pssz>;
         constexpr std::size_t W = [] {
            if constexpr (std::is_same_v<Fmt, ::psio::pssz>)
               return ::psio::auto_pssz_width<T>();
            else
               return Fmt::offset_width;
         }();
         const std::size_t total = size_of_v<W, Auto>(value);
         enforce_max_dynamic<T>(total);
         std::vector<char> out(total);
         ::psio::fast_buf_stream sink(out.data(), total);
         encode_value<W, Auto>(value, sink);
         return out;
      }
   }  // namespace detail::pssz_impl

   // ── Checked encode ────────────────────────────────────────────────────
   //
   // Appends the encoding of `value` to `out`, or returns why the value has
   // none and leaves `out` unchanged: node depth, a null Box, a utf8_string
   // that is not UTF-8 or over its bound, a member outside its length_bound,
   // a bitlist over its capacity, a value (nested ones included) that
   // violates its type's invariant — reported with the invariant's own
   // status —, a total over the type's maxDynamicData
   // cap, a nested record over its own cap, a payload that does not fit the
   // type's static offset width or the 32-bit address space, or a nonempty
   // vector of zero-size elements. It never throws and never aborts on a
   // value: usable with exceptions disabled. (Allocation failure and a
   // user-written binary adapter that throws are outside its control.)
   namespace detail::pssz_impl
   {
      template <typename T, std::size_t W>
      codec_status checked_size(pssz_<W>, const T& value, std::size_t& total)
      {
         size_check check;
         total = size_of_v<W, false>(value, &check);
         enforce_max_dynamic<T>(total, &check);
         return check.failure ? codec_fail(check.failure, 0, "pssz") : codec_ok();
      }
      template <typename T>
      codec_status checked_size(pssz, const T& value, std::size_t& total)
      {
         size_check check;
         total = size_of_v<auto_pssz_width_v<T>, true>(value, &check);
         enforce_max_dynamic<T>(total, &check);
         return check.failure ? codec_fail(check.failure, 0, "pssz") : codec_ok();
      }
   }  // namespace detail::pssz_impl

   namespace detail::pssz_impl
   {
      // The checked encoder, with the preflight it is asked for. One sizing
      // pass in checked mode; once it succeeds the write repeats exactly the
      // same arithmetic on the same value and cannot fail.
      template <typename T, typename Fmt>
      codec_status encode_checked(Fmt format, const T& value, std::vector<char>& out,
                                  encode_preflight preflight)
      {
         if (auto status = check_encode_depth_impl(value, 0, preflight); !status.ok())
            return status;
         std::size_t total = 0;
         if (auto status = checked_size(format, value, total); !status.ok())
            return status;
         if (total > std::numeric_limits<std::size_t>::max() - out.size())
            return codec_fail("pssz: encoded size overflow", 0, "pssz");
         constexpr bool        Auto = std::is_same_v<Fmt, ::psio::pssz>;
         constexpr std::size_t W    = [] {
            if constexpr (std::is_same_v<Fmt, ::psio::pssz>)
               return ::psio::auto_pssz_width<T>();
            else
               return Fmt::offset_width;
         }();
         const std::size_t orig = out.size();
         out.resize(orig + total);
         ::psio::fast_buf_stream sink(out.data() + orig, total);
         encode_value<W, Auto>(value, sink);
         return codec_ok();
      }

      // The frame of a value whose invariants (invariant.hpp) are NOT
      // checked; everything else is. For the two callers that own that
      // check themselves: a custom handler's from_text, whose `check` runs
      // next (pssz_ir_json.hpp), and tests that need the frame of a value
      // that violates an invariant — which every checking reader rejects.
      template <typename T, typename Fmt>
      [[nodiscard]] codec_status encode_ignoring_invariants(Fmt format, const T& value,
                                                            std::vector<char>& out)
      {
         return encode_checked(format, value, out, encode_preflight{true, false});
      }
   }  // namespace detail::pssz_impl

   template <typename T, typename Fmt>
   [[nodiscard]] codec_status encode_pssz_checked(Fmt format, const T& value,
                                                  std::vector<char>& out)
   {
      return detail::pssz_impl::encode_checked(format, value, out, {});
   }

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

#include <psio/detail/pssz_assoc.hpp>
