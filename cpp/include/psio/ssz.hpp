#pragma once
//
// psio/ssz.hpp — SSZ (Simple Serialize) format tag.
//
// SSZ is the Ethereum consensus-layer canonical format. This header
// implements the v3-surface SSZ codec: `psio::ssz` is the format tag,
// with hidden-friend tag_invoke overloads for every CPO
// (encode/decode/size_of/validate/validate_strict).
//
// Scope (Phase 6 MVP) — types supported here:
//   - bool, integer primitives (u8..u64, i8..i64), float, double
//   - std::array<T, N>             (SSZ Vector[T, N])
//   - std::vector<T>               (SSZ List[T, *])
//   - std::string                  (byte-list)
//   - std::optional<T>             (0 bytes for none; encoded payload for some)
//   - Records reflected via PSIO_REFLECT (SSZ Container)
//
// Out of scope for the first cut (land in follow-ups):
//   uint256 / __int128, std::bitset / bitvector / bitlist, std::variant
//   (SSZ Union), wrappers like bounded<T, N>, schema-bound validation.
//
// Wire rules reproduced verbatim from the v1 SSZ implementation (see
// libraries/psio1/cpp/include/psio/{to,from}_ssz.hpp). Integers are raw
// little-endian; vectors/arrays of fixed-size elements are packed tight;
// vectors of variable-size elements use a front offset table of uint32
// slot offsets (relative to the start of the container), with payloads
// appended after. Containers follow the same fixed/variable split.

#include <psio/cpo.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/validate_strict_walker.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <variant>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio {

   struct ssz;  // fwd — used by adapter-dispatch trait below.

   namespace detail::ssz_impl {

      // ── Shape classification ──────────────────────────────────────────────
      //
      // "Fixed-size" (in SSZ parlance) means the wire length is a
      // compile-time constant. Primitives, arrays of fixed elements, and
      // reflected records all-of-fixed-fields are fixed. std::vector,
      // std::string, std::optional, and any record containing a variable
      // field are variable.

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

      // psio::bitvector<N>: SSZ Bitvector[N], ceil(N/8) bytes, fixed.
      template <std::size_t N>
      struct is_fixed<::psio::bitvector<N>> : std::true_type
      {
      };
      // psio::bitlist<MaxN>: SSZ Bitlist[N], variable (trailing
      // delimiter bit determines the bit count at decode time).

      // 128/256-bit integers — fixed-width raw LE bytes.
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

      // std::vector, std::string, std::optional are variable.
      // Reflected records: fixed iff every field is fixed.

      template <typename T>
      inline constexpr bool is_fixed_v = is_fixed<T>::value;

      template <typename T>
      concept Record = ::psio::Reflected<T>;

      // Helper: does every reflected field of T have is_fixed_v == true?
      template <Record T, std::size_t... Is>
      consteval bool record_fields_all_fixed(std::index_sequence<Is...>)
      {
         using R = ::psio::reflect<T>;
         return (is_fixed_v<typename R::template member_type<Is>> && ...);
      }

      template <Record T>
      consteval bool record_all_fixed()
      {
         using R = ::psio::reflect<T>;
         return record_fields_all_fixed<T>(
            std::make_index_sequence<R::member_count>{});
      }

      // Projected types are runtime-sized opaque bytes — not fixed.
      template <typename T>
      inline constexpr bool has_binary_adapter_v =
         ::psio::has_adapter_v<T, ::psio::binary_category>;

      template <Record T>
         requires(!has_binary_adapter_v<T>)
      struct is_fixed<T>
         : std::bool_constant<record_all_fixed<T>()>
      {
      };

      template <typename T>
         requires(has_binary_adapter_v<T>)
      struct is_fixed<T> : std::false_type
      {
      };

      // ── Fixed-size byte count ─────────────────────────────────────────────

      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept;

      template <Record T, std::size_t... Is>
      consteval std::size_t sum_record_fields(std::index_sequence<Is...>)
      {
         using R = ::psio::reflect<T>;
         return (fixed_size_of<typename R::template member_type<Is>>() + ... + 0);
      }

      // Size contribution of member I to the SSZ record's fixed_region:
      // fixed members contribute their full payload, variable members
      // contribute a 4-byte offset slot.  `_full` additionally honours
      // the `as_override` annotation, which forces a member to be
      // treated as variable regardless of its underlying type.  These
      // are free function templates rather than the previous
      // lambda-in-fold pattern because clang 22 trips on
      // `lambda.template operator()<I>()` invoked from a fold inside a
      // non-constant-evaluated enclosing function.
      template <Record T, std::size_t I>
      consteval std::size_t ssz_member_fixed_size_simple()
      {
         using R = ::psio::reflect<T>;
         using F = typename R::template member_type<I>;
         if constexpr (is_fixed_v<F>) return fixed_size_of<F>();
         else                          return 4;
      }

      template <Record T, std::size_t I>
      consteval std::size_t ssz_member_fixed_size_full()
      {
         using R = ::psio::reflect<T>;
         using F = typename R::template member_type<I>;
         using eff = typename ::psio::effective_annotations_for<
            T, F, R::template member_pointer<I>>::value_t;
         constexpr bool override_v = ::psio::has_as_override_v<eff>;
         if constexpr (!override_v && is_fixed_v<F>)
            return fixed_size_of<F>();
         else
            return 4;
      }

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

      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept
      {
         if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            return 16;
         else if constexpr (std::is_arithmetic_v<T>)
            return sizeof(T);
         else if constexpr (is_bitvector_v<T>)
            return (T::size_value + 7) / 8;
         else if constexpr (requires { typename T::value_type; } &&
                            requires { T{}.size(); })
         {
            // std::array<T, N>: N * fixed_size_of<T>
            using E = typename T::value_type;
            return std::tuple_size<T>::value * fixed_size_of<E>();
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            return sum_record_fields<T>(
               std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return 0;  // unreachable for is_fixed_v<T> types
         }
      }

      // ── Dynamic size probe (for variable types) ───────────────────────────
      //
      // Forward-declared traits (is_std_array_v / is_std_vector_v /
      // is_std_optional_v) live below the encode section; forward-declare
      // them here so size_of_v can use them in its constexpr-if.

      template <typename T>
      struct is_std_array;
      template <typename T>
      struct is_std_vector;
      template <typename T>
      struct is_std_optional;

      template <typename T>
      std::size_t size_of_v(const T& v) noexcept
      {
         if constexpr (is_fixed_v<T>)
         {
            return fixed_size_of<T>();
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
               return N * fixed_size_of<E>();
            else
            {
               std::size_t total = N * 4;
               for (const auto& x : v)
                  total += size_of_v(x);
               return total;
            }
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
               return v.size() * fixed_size_of<E>();
            else
            {
               std::size_t total = v.size() * 4;
               for (const auto& x : v)
                  total += size_of_v(x);
               return total;
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            return v.size();
         }
         else if constexpr (is_std_optional<T>::value)
         {
            // SSZ optional = Union[null, T]: 1-byte selector + payload.
            return 1 + (v.has_value() ? size_of_v(*v) : 0);
         }
         else if constexpr (is_bitlist_v<T>)
         {
            // SSZ bitlist: (bit_count + 8) / 8 bytes — data bytes
            // plus one for the trailing delimiter bit.
            return (v.size() + 8) / 8;
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // SSZ Union: 1 byte selector + size of the chosen alternative.
            std::size_t total = 1;
            std::visit([&](const auto& alt) { total += size_of_v(alt); }, v);
            return total;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) noexcept {
               std::size_t total = 0;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);
                      if constexpr (is_fixed_v<F>)
                         total += fixed_size_of<F>();
                      else
                         total += 4 + size_of_v(fref);
                   }()),
                  ...);
               return total;
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::ssz: size_of_v: unsupported type");
            return 0;
         }
      }

      // ── Encoding ──────────────────────────────────────────────────────────
      //
      // Sink is `std::vector<char>` throughout — simplest interface that
      // satisfies backpatching (we write offsets by updating a known index
      // after the tail has been emitted).

      using sink_t = std::vector<char>;

      // Trait helpers for the single-dispatcher encode_value.
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

      // Single dispatcher — the only encode_value template. All per-type
      // logic is gated by `if constexpr` so name lookup at template-parse
      // time only needs to find this function, not per-type overloads.
      //
      // Sink-templated: callers pass either ::psio::size_stream (size
      // probe) or ::psio::fast_buf_stream (write into a pre-sized
      // buffer). Top-level tag_invoke runs both passes — same code,
      // mirrors v1's convert_to_ssz pattern.
      template <typename T, typename Sink>
      void encode_value(const T& v, Sink& s)
      {
         // Adapter dispatch — projected types emit their opaque
         // bytes; SSZ's container walkers treat the result as a
         // variable-size payload (offset-addressed) through the same
         // heap mechanism they use for strings and vectors. Adapters
         // assume a vector<char>-like sink, so we route through a
         // temp vector and then forward the bytes through the
         // Sink-uniform write interface.
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::ssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            sink_t tmp;
            Proj::encode(v, tmp);
            s.write(tmp.data(), tmp.size());
            return;
         }

         if constexpr (std::is_same_v<T, bool>)
         {
            s.write(v ? '\x01' : '\x00');
         }
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            s.write(v.limb, 32);
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
         {
            s.write(&v, 16);
         }
         else if constexpr (std::is_arithmetic_v<T>)
         {
            s.put(v);
         }
         else if constexpr (is_std_array_v<T>)
         {
            using E                    = typename T::value_type;
            constexpr std::size_t N    = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
            {
               if constexpr (std::is_arithmetic_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  if constexpr (N > 0)
                     s.write(v.data(), N * sizeof(E));
               }
               else
               {
                  for (const auto& x : v)
                     encode_value(x, s);
               }
            }
            else
            {
               const std::size_t table_index = s.written();
               s.skip(static_cast<std::int32_t>(N * 4));
               for (std::size_t i = 0; i < N; ++i)
               {
                  const std::uint32_t rel =
                     static_cast<std::uint32_t>(s.written() - table_index);
                  s.rewrite(table_index + i * 4, &rel, 4);
                  encode_value(v[i], s);
               }
            }
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               // Bulk-memcpy fast path covers two cases:
               //   (a) arithmetic primitives — raw LE bytes
               //   (b) trivially-copyable records with sizeof(E) ==
               //       fixed_size_of<E>() — packed memory == wire
               // Matches v1's ssz_memcpy_ok_v path. Big win on
               // vec<Validator>×N workloads.
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
                     encode_value(x, s);
               }
            }
            else
            {
               const std::size_t n           = v.size();
               const std::size_t table_index = s.written();
               s.skip(static_cast<std::int32_t>(n * 4));
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::uint32_t rel =
                     static_cast<std::uint32_t>(s.written() - table_index);
                  s.rewrite(table_index + i * 4, &rel, 4);
                  encode_value(v[i], s);
               }
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            s.write(v.data(), v.size());
         }
         else if constexpr (is_std_optional_v<T>)
         {
            // SSZ encodes optional as Union[null, T]:
            //   None    → 0x00
            //   Some(x) → 0x01 || serialized(x)
            // Matches v1 wire format (canonical SSZ Union).
            if (v.has_value())
            {
               s.write('\x01');
               encode_value(*v, s);
            }
            else
            {
               s.write('\x00');
            }
         }
         else if constexpr (is_bitvector_v<T>)
         {
            // SSZ bitvector: ceil(N/8) raw bytes, LSB-first. psio's
            // bitvector storage already matches the wire layout.
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(v.data(), nbytes);
         }
         else if constexpr (is_bitlist_v<T>)
         {
            // SSZ bitlist: data bits + trailing '1' delimiter at the
            // bit_count-th position. Total = (bit_count + 8) / 8.
            // Stage in a stack/heap buffer so we can write through
            // the sink with a single write call (works for both
            // size_stream and fast_buf_stream).
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
            // SSZ Union: 1-byte selector + value (canonical). Variant
            // is always variable — records addressing it go through the
            // offset table (see record walker, variable-field branch).
            static_assert(std::variant_size_v<T> <= 256,
                          "ssz variant selector is u8 (≤ 256 alternatives)");
            s.write(static_cast<char>(v.index()));
            std::visit([&](const auto& alt) { encode_value(alt, s); }, v);
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path for trivially-copyable records whose
            // memory layout matches the wire (sizeof(T) ==
            // fixed_size_of<T>()). Nested-constexpr to avoid evaluating
            // fixed_size_of<T> for non-fixed types (it can hit the
            // std::array branch which depends on tuple_size<T>).
            if constexpr (std::is_trivially_copyable_v<T> && is_fixed_v<T>)
            {
               if constexpr (fixed_size_of<T>() == sizeof(T))
               {
                  s.write(&v, sizeof(T));
                  return;
               }
            }
            using R = ::psio::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               // fixed_region is a compile-time constant for any
               // reflected T — sum of fixed field sizes + 4 per
               // variable field. Hoist to constexpr.
               constexpr std::size_t fixed_region =
                  (ssz_member_fixed_size_full<T, Is>() + ... + std::size_t{0});
               const std::size_t container_start = s.written();
               // Reserve the fixed region — for size_stream this is
               // size += fixed_region; for fast_buf_stream this is
               // pos += fixed_region (no zero-fill — every byte is
               // written via rewrite below).
               s.skip(static_cast<std::int32_t>(fixed_region));
               std::size_t fixed_cursor = container_start;

               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);
                      using eff =
                         typename ::psio::effective_annotations_for<
                            T, F,
                            R::template member_pointer<Is>>::value_t;
                      constexpr bool override_v =
                         ::psio::has_as_override_v<eff>;

                      if constexpr (!override_v && is_fixed_v<F>)
                      {
                         // Encode the fixed field through a tiny temp
                         // sub-stream that wraps the same Sink with
                         // its position pinned at fixed_cursor. For
                         // size_stream this collapses to no-op writes;
                         // for fast_buf_stream the rewrites land at
                         // begin + fixed_cursor.
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
                               // Nested non-arithmetic array: encode
                               // into a temp vector then rewrite.
                               sink_t tmp;
                               ::psio::vector_stream vs{tmp};
                               encode_value(fref, vs);
                               s.rewrite(fixed_cursor, tmp.data(),
                                         tmp.size());
                            }
                         }
                         else
                         {
                            // Fallback for nested fixed records.
                            sink_t tmp;
                            ::psio::vector_stream vs{tmp};
                            encode_value(fref, vs);
                            s.rewrite(fixed_cursor, tmp.data(),
                                      tmp.size());
                         }
                         fixed_cursor += fixed_size_of<F>();
                      }
                      else
                      {
                         const std::uint32_t rel = static_cast<std::uint32_t>(
                            s.written() - container_start);
                         s.rewrite(fixed_cursor, &rel, 4);
                         fixed_cursor += 4;
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
                            encode_value(fref, s);
                         }
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::ssz: encode_value: unsupported type");
         }
      }

      // ── Decoding ──────────────────────────────────────────────────────────
      //
      // All decoders take [pos, end) — pos is the byte index of the value's
      // start within the full input buffer, end is the byte index of the
      // value's end. Fixed decoders ignore `end`; variable decoders use it.

      template <typename T>
      T decode_value(std::span<const char> src,
                     std::size_t            pos,
                     std::size_t            end);

      template <Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src,
                              std::size_t            pos,
                              std::size_t            end,
                              T&                     out,
                              std::index_sequence<Is...>);

      // In-place decode for std::string, bulk-memcpy std::vector,
      // and nested Records. Records dispatch to record_decode_into
      // directly, avoiding the temp + move-assign on every nested
      // record field — material on shapes like Order which has a
      // nested UserProfile with two strings.
      template <typename T>
      void decode_into(std::span<const char> src, std::size_t pos,
                       std::size_t end, T& out)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            // SSZ string: raw bytes, length = end - pos.
            out.assign(src.data() + pos, src.data() + end);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               constexpr bool is_arith =
                  std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
               constexpr bool is_memcpy_record =
                  Record<E> && std::is_trivially_copyable_v<E> &&
                  fixed_size_of<E>() == sizeof(E);
               if constexpr (is_arith || is_memcpy_record)
               {
                  const std::size_t n = (end - pos) / sizeof(E);
                  const E*          first =
                     reinterpret_cast<const E*>(src.data() + pos);
                  out.assign(first, first + n);
                  return;
               }
            }
            out = decode_value<T>(src, pos, end);
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path mirrors decode_value's; for non-memcpy
            // records, dispatch straight to the in-place walker.
            if constexpr (std::is_trivially_copyable_v<T> &&
                          is_fixed_v<T>)
               if constexpr (fixed_size_of<T>() == sizeof(T))
            {
               std::memcpy(&out, src.data() + pos, sizeof(T));
               return;
            }
            using R = ::psio::reflect<T>;
            record_decode_into<T>(
               src, pos, end, out,
               std::make_index_sequence<R::member_count>{});
         }
         else
         {
            out = decode_value<T>(src, pos, end);
         }
      }

      template <typename T>
         requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
      T decode_arith(std::span<const char> src, std::size_t pos)
      {
         T out{};
         std::memcpy(&out, src.data() + pos, sizeof(T));
         return out;
      }

      template <typename T, std::size_t N>
      std::array<T, N> decode_array(std::span<const char> src,
                                    std::size_t            pos,
                                    std::size_t            end)
      {
         std::array<T, N> out{};
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            for (std::size_t i = 0; i < N; ++i)
               out[i] = decode_value<T>(src, pos + i * esz, pos + (i + 1) * esz);
         }
         else
         {
            if constexpr (N > 0)
            {
               std::array<std::uint32_t, N> offsets{};
               for (std::size_t i = 0; i < N; ++i)
                  std::memcpy(&offsets[i], src.data() + pos + i * 4, 4);
               for (std::size_t i = 0; i < N; ++i)
               {
                  const std::size_t beg = pos + offsets[i];
                  const std::size_t fin =
                     (i + 1 < N) ? (pos + offsets[i + 1]) : end;
                  out[i] = decode_value<T>(src, beg, fin);
               }
            }
         }
         return out;
      }

      // Forward declaration — used by decode_vector to decode record
      // elements in-place. Definition appears later in this file.
      template <Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src,
                              std::size_t            pos,
                              std::size_t            end,
                              T&                     out,
                              std::index_sequence<Is...>);

      template <typename T>
      std::vector<T> decode_vector(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end)
      {
         std::vector<T> out;
         if (pos >= end)
            return out;
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            const std::size_t n   = (end - pos) / esz;
            // Bulk-memcpy fast path covers both arithmetic primitives AND
            // DWNC packed records whose memory layout matches the wire
            // layout (sizeof(T) == fixed_size_of<T>() and trivially-
            // copyable). assign(p, p+n) lowers to a single memcpy of
            // n*sizeof(T) bytes, skipping the resize+zero-init pass.
            constexpr bool is_arith =
               std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;
            constexpr bool is_memcpy_record =
               Record<T> && std::is_trivially_copyable_v<T> &&
               fixed_size_of<T>() == sizeof(T);
            if constexpr (is_arith || is_memcpy_record)
            {
               const T* first = reinterpret_cast<const T*>(src.data() + pos);
               out.assign(first, first + n);
            }
            else if constexpr (Record<T>)
            {
               // In-place decode — resize once (single bulk zero-init)
               // then write each element directly into its slot. Avoids
               // the per-element `T tmp{}` + move-construct that
               // push_back(decode_value<T>(...)) incurs.
               out.resize(n);
               using R = ::psio::reflect<T>;
               for (std::size_t i = 0; i < n; ++i)
                  record_decode_into<T>(
                     src, pos + i * esz, pos + (i + 1) * esz, out[i],
                     std::make_index_sequence<R::member_count>{});
            }
            else
            {
               out.reserve(n);
               for (std::size_t i = 0; i < n; ++i)
                  out.push_back(
                     decode_value<T>(src, pos + i * esz, pos + (i + 1) * esz));
            }
         }
         else
         {
            // First offset tells us the count.
            if (end - pos < 4)
               return out;
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            const std::size_t n = first / 4;
            std::vector<std::uint32_t> offsets(n);
            for (std::size_t i = 0; i < n; ++i)
               std::memcpy(&offsets[i], src.data() + pos + i * 4, 4);
            if constexpr (Record<T>)
            {
               // In-place: resize then walk each element directly into
               // its slot. Avoids the move-construct on push_back +
               // the temp returned by decode_value.
               out.resize(n);
               using R = ::psio::reflect<T>;
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::size_t beg = pos + offsets[i];
                  const std::size_t fin =
                     (i + 1 < n) ? (pos + offsets[i + 1]) : end;
                  record_decode_into<T>(
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
                  out.push_back(decode_value<T>(src, beg, fin));
               }
            }
         }
         return out;
      }

      inline std::string decode_string(std::span<const char> src,
                                       std::size_t            pos,
                                       std::size_t            end)
      {
         return std::string(src.data() + pos, src.data() + end);
      }

      template <typename T>
      std::optional<T> decode_optional(std::span<const char> src,
                                       std::size_t            pos,
                                       std::size_t            end)
      {
         // SSZ Union[null, T] decoding: first byte is the selector.
         // 0x00 → None, 0x01 → Some(payload follows).
         if (pos >= end)
            return std::nullopt;
         const std::uint8_t sel =
            static_cast<unsigned char>(src[pos]);
         if (sel == 0x00)
            return std::nullopt;
         // sel == 0x01 (or anything non-zero for tolerant decode);
         // payload occupies [pos+1, end).
         return decode_value<T>(src, pos + 1, end);
      }

      // Decode in-place: write fields directly into `out`. Mirrors v1's
      // from_ssz(T&, ...) path. Used by decode_vector to avoid the
      // per-element `T tmp{}` + move-construct cost when filling a
      // pre-resized destination.
      template <Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src,
                              std::size_t            pos,
                              std::size_t            end,
                              T&                     out,
                              std::index_sequence<Is...>)
      {
         using R = ::psio::reflect<T>;

         std::array<std::uint32_t, R::member_count> var_offsets{};
         std::array<bool, R::member_count>          is_var{};
         std::size_t                                 cursor = pos;
         (
            ([&]
             {
                using F = typename R::template member_type<Is>;
                using eff =
                   typename ::psio::effective_annotations_for<
                      T, F, R::template member_pointer<Is>>::value_t;
                constexpr bool override_v =
                   ::psio::has_as_override_v<eff>;
                if constexpr (!override_v && is_fixed_v<F>)
                {
                   is_var[Is]       = false;
                   var_offsets[Is]  = 0;
                   cursor          += fixed_size_of<F>();
                }
                else
                {
                   is_var[Is] = true;
                   std::uint32_t off = 0;
                   std::memcpy(&off, src.data() + cursor, 4);
                   var_offsets[Is]  = off;
                   cursor          += 4;
                }
             }()),
            ...);

         std::array<std::size_t, R::member_count> var_end{};
         {
            std::size_t last_end = end;
            for (std::size_t i = R::member_count; i-- > 0;)
            {
               if (is_var[i])
               {
                  var_end[i] = last_end;
                  last_end   = pos + var_offsets[i];
               }
            }
         }

         std::size_t fixed_cursor = pos;
         (
            ([&]
             {
                using F = typename R::template member_type<Is>;
                auto& fref = out.*(R::template member_pointer<Is>);
                using eff =
                   typename ::psio::effective_annotations_for<
                      T, F, R::template member_pointer<Is>>::value_t;
                constexpr bool override_v =
                   ::psio::has_as_override_v<eff>;

                if constexpr (!override_v && is_fixed_v<F>)
                {
                   fref = decode_value<F>(src, fixed_cursor,
                                          fixed_cursor + fixed_size_of<F>());
                   fixed_cursor += fixed_size_of<F>();
                }
                else
                {
                   const std::size_t beg = pos + var_offsets[Is];
                   if constexpr (override_v)
                   {
                      using Tag = ::psio::adapter_tag_of_t<eff>;
                      using Proj = ::psio::adapter<
                         std::remove_cvref_t<F>, Tag>;
                      fref = Proj::decode(std::span<const char>(
                         src.data() + beg, var_end[Is] - beg));
                   }
                   else
                   {
                      decode_into<F>(src, beg, var_end[Is], fref);
                   }
                   fixed_cursor += 4;
                }
             }()),
            ...);
      }

      template <Record T, std::size_t... Is>
      T record_decode(std::span<const char> src,
                      std::size_t            pos,
                      std::size_t            end,
                      std::index_sequence<Is...> seq)
      {
         T out{};
         record_decode_into<T>(src, pos, end, out, seq);
         return out;
      }

      // ── decode_value dispatch ─────────────────────────────────────────────
      //
      // Single generic entry that selects the right specialization.

      template <typename T>
      T decode_value(std::span<const char> src,
                     std::size_t            pos,
                     std::size_t            end)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::ssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            return Proj::decode(
               std::span<const char>(src.data() + pos, end - pos));
         }

         if constexpr (std::is_same_v<T, bool>)
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
            return decode_arith<T>(src, pos);
         else if constexpr (std::is_same_v<T, std::string>)
            return decode_string(src, pos, end);
         else if constexpr (requires { typename T::value_type; } &&
                            requires { std::tuple_size<T>::value; })
         {
            return decode_array<typename T::value_type,
                                std::tuple_size<T>::value>(src, pos, end);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // SSZ Union: 1-byte selector + value (value occupies
            // [pos+1, end)).
            const auto idx = static_cast<std::size_t>(
               static_cast<unsigned char>(src[pos]));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
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
            // Walk the trailing delimiter bit: scan the last byte
            // high-to-low to find the highest set bit; that's the
            // end marker and the bit positions below it are data.
            T out;
            if (end <= pos)
               return out;
            const std::size_t total_bytes = end - pos;
            // Find delimiter in the last byte.
            const unsigned char last =
               static_cast<unsigned char>(src[end - 1]);
            if (last == 0)
               return out;  // malformed; treat as empty
            int delim_bit = 7;
            while (delim_bit >= 0 && !((last >> delim_bit) & 1))
               --delim_bit;
            const std::size_t bit_count =
               (total_bytes - 1) * 8 + static_cast<std::size_t>(delim_bit);
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
         else if constexpr (requires { typename T::value_type; } &&
                            !Record<T> &&
                            !std::is_same_v<T, std::string>)
         {
            using V = typename T::value_type;
            if constexpr (requires(T t) { t.has_value(); })
               return decode_optional<V>(src, pos, end);
            else
               return decode_vector<V>(src, pos, end);
         }
         else if constexpr (Record<T>)
         {
            // Memcpy fast path for trivially-copyable records whose
            // memory layout matches the wire. Nested if-constexpr so
            // fixed_size_of<T> isn't evaluated when T is not is_fixed.
            if constexpr (std::is_trivially_copyable_v<T> && is_fixed_v<T>)
            {
               if constexpr (fixed_size_of<T>() == sizeof(T))
               {
                  T out;
                  std::memcpy(&out, src.data() + pos, sizeof(T));
                  return out;
               }
            }
            using R = ::psio::reflect<T>;
            return record_decode<T>(src, pos, end,
                                    std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0, "psio::ssz: unsupported type");
         }
      }

      // ── Structural validation ─────────────────────────────────────────────
      //
      // Full byte-level walker. Mirrors v1's `ssz_validate_impl`: every
      // offset is range-checked and every variable-element child is
      // recursively validated, so `validate(buffer)` returning ok() is
      // a sufficient precondition for `decode<T>(buffer)` to be safe
      // on adversarial input.
      //
      // Semantic checks (UTF-8 well-formedness, length bounds, hex
      // digits, ...) are layered on top by `validate_strict<T>`, which
      // calls structural `validate_value` first then runs the
      // user-declared spec walkers from validate_strict_walker.hpp.

      template <typename T>
      codec_status validate_value(std::span<const char> src,
                                  std::size_t            pos,
                                  std::size_t            end) noexcept;

      // ── Vector / Array helpers ────────────────────────────────────────────

      template <typename E>
      codec_status validate_vector_payload(std::span<const char> src,
                                           std::size_t            pos,
                                           std::size_t            end) noexcept
      {
         if (pos > end || end > src.size())
            return codec_fail("ssz: vector span out of bounds",
                              static_cast<std::uint32_t>(pos), "ssz");
         if (pos == end)
            return codec_ok();

         if constexpr (is_fixed_v<E>)
         {
            const std::size_t esz = fixed_size_of<E>();
            if ((end - pos) % esz != 0)
               return codec_fail("ssz: vector span not a multiple of "
                                 "element size",
                                 static_cast<std::uint32_t>(pos), "ssz");
            // Fixed-size primitives only need the divisibility check —
            // every byte is a valid LE value. Memcpy-layout records
            // (DWNC + trivially-copyable + sizeof == fixed_size) have
            // wire == memory bytes, no offsets to check. Other Records
            // may carry internal offsets / length prefixes that need
            // recursive validation per element.
            constexpr bool memcpy_layout =
               Record<E> && std::is_trivially_copyable_v<E> &&
               fixed_size_of<E>() == sizeof(E);
            if constexpr (Record<E> && !memcpy_layout)
            {
               const std::size_t n = (end - pos) / esz;
               for (std::size_t i = 0; i < n; ++i)
               {
                  auto st = validate_value<E>(src, pos + i * esz,
                                              pos + (i + 1) * esz);
                  if (!st.ok()) return st;
               }
            }
            return codec_ok();
         }
         else
         {
            // Variable-element list: front offset table.
            //   Layout: [u32 off_0][u32 off_1]...[u32 off_n-1][payload_0]...
            //   off_0 doubles as 4*n (count derivation).
            if (end - pos < 4)
               return codec_fail("ssz: variable list missing offset table",
                                 static_cast<std::uint32_t>(pos), "ssz");
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            if (first % 4 != 0 || first > span)
               return codec_fail("ssz: invalid first list offset",
                                 static_cast<std::uint32_t>(pos), "ssz");
            const std::size_t n = first / 4;
            if (span < n * 4)
               return codec_fail("ssz: list offset table truncated",
                                 static_cast<std::uint32_t>(pos), "ssz");
            // Read all offsets, ensure non-decreasing and within span.
            std::uint32_t prev = first;
            for (std::size_t i = 1; i < n; ++i)
            {
               std::uint32_t off_i = 0;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (off_i < prev || off_i > span)
                  return codec_fail("ssz: list offset out of range",
                                    static_cast<std::uint32_t>(pos + i * 4),
                                    "ssz");
               prev = off_i;
            }
            // Recurse into each element with its derived span.
            for (std::size_t i = 0; i < n; ++i)
            {
               std::uint32_t off_i = 0, stop = span;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (i + 1 < n)
                  std::memcpy(&stop, src.data() + pos + (i + 1) * 4, 4);
               auto st = validate_value<E>(src, pos + off_i, pos + stop);
               if (!st.ok()) return st;
            }
            return codec_ok();
         }
      }

      template <typename E, std::size_t N>
      codec_status validate_array_payload(std::span<const char> src,
                                          std::size_t            pos,
                                          std::size_t            end) noexcept
      {
         // SSZ Vector[E, N] (std::array). Wire layout differs from
         // List[E]: no front count (N is part of the type).
         if constexpr (is_fixed_v<E>)
         {
            constexpr std::size_t esz = fixed_size_of<E>();
            if (end - pos < N * esz)
               return codec_fail("ssz: array buffer too small",
                                 static_cast<std::uint32_t>(pos), "ssz");
            constexpr bool memcpy_layout =
               Record<E> && std::is_trivially_copyable_v<E> &&
               fixed_size_of<E>() == sizeof(E);
            if constexpr (Record<E> && !memcpy_layout)
            {
               for (std::size_t i = 0; i < N; ++i)
               {
                  auto st = validate_value<E>(src, pos + i * esz,
                                              pos + (i + 1) * esz);
                  if (!st.ok()) return st;
               }
            }
            return codec_ok();
         }
         else
         {
            // Variable-element fixed-N array: N offsets, no count
            // prefix (count is the type N).
            if constexpr (N == 0)
               return codec_ok();
            if (end - pos < N * 4)
               return codec_fail("ssz: array offset table truncated",
                                 static_cast<std::uint32_t>(pos), "ssz");
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            if (first != N * 4 || first > span)
               return codec_fail("ssz: invalid array first offset",
                                 static_cast<std::uint32_t>(pos), "ssz");
            std::uint32_t prev = first;
            for (std::size_t i = 1; i < N; ++i)
            {
               std::uint32_t off_i = 0;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (off_i < prev || off_i > span)
                  return codec_fail("ssz: array offset out of range",
                                    static_cast<std::uint32_t>(pos + i * 4),
                                    "ssz");
               prev = off_i;
            }
            for (std::size_t i = 0; i < N; ++i)
            {
               std::uint32_t off_i = 0, stop = span;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (i + 1 < N)
                  std::memcpy(&stop, src.data() + pos + (i + 1) * 4, 4);
               auto st = validate_value<E>(src, pos + off_i, pos + stop);
               if (!st.ok()) return st;
            }
            return codec_ok();
         }
      }

      // ── Record walker ─────────────────────────────────────────────────────

      template <Record T, std::size_t... Is>
      codec_status validate_record(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end,
                                   std::index_sequence<Is...>) noexcept
      {
         using R = ::psio::reflect<T>;
         constexpr std::size_t NF = R::member_count;

         // Compile-time fixed_region size: sum of fixed field sizes
         // plus 4 per variable field (offset slot).
         constexpr std::size_t fixed_region = (
            ssz_member_fixed_size_simple<T, Is>() + ... + std::size_t{0});

         if (end - pos < fixed_region)
            return codec_fail("ssz: record fixed region truncated",
                              static_cast<std::uint32_t>(pos), "ssz");

         if constexpr (is_fixed_v<T>)
         {
            // Fixed record: walk fields, recursive validate. fixed_cursor
            // advances by each field's fixed size.
            std::size_t  fixed_cursor = pos;
            codec_status err          = codec_ok();
            (
               ([&]
                {
                   if (!err.ok()) return;
                   using F = typename R::template member_type<Is>;
                   constexpr std::size_t fs = fixed_size_of<F>();
                   auto st = validate_value<F>(src, fixed_cursor,
                                               fixed_cursor + fs);
                   if (!st.ok()) { err = std::move(st); return; }
                   fixed_cursor += fs;
                }()),
               ...);
            return err;
         }
         else
         {
            // Variable record: collect offsets, validate offset table,
            // then recurse into each variable field with its derived span.
            std::array<std::uint32_t, NF> offsets{};
            std::array<bool, NF>          is_var{};
            std::size_t                   fp = pos;
            (
               ([&]
                {
                   using F = typename R::template member_type<Is>;
                   if constexpr (is_fixed_v<F>)
                   {
                      is_var[Is] = false;
                      fp        += fixed_size_of<F>();
                   }
                   else
                   {
                      is_var[Is]   = true;
                      std::uint32_t o = 0;
                      std::memcpy(&o, src.data() + fp, 4);
                      offsets[Is]  = o;
                      fp          += 4;
                   }
                }()),
               ...);

            // Offsets are pointer-relative to `pos` (container start).
            // Each must be >= fixed_region and >= the previous variable
            // offset, and <= container span.
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            std::uint32_t prev = static_cast<std::uint32_t>(fixed_region);
            for (std::size_t i = 0; i < NF; ++i)
            {
               if (!is_var[i]) continue;
               if (offsets[i] < prev || offsets[i] > span)
                  return codec_fail("ssz: record variable offset out of range",
                                    static_cast<std::uint32_t>(pos), "ssz");
               prev = offsets[i];
            }

            // Compute each variable field's stop = next variable offset
            // (or span if last). Walk fields again, recursive validate
            // on each variable child.
            std::array<std::uint32_t, NF> var_end{};
            {
               std::uint32_t last_end = span;
               for (std::size_t i = NF; i-- > 0;)
               {
                  if (is_var[i])
                  {
                     var_end[i] = last_end;
                     last_end   = offsets[i];
                  }
               }
            }

            // First-offset-must-equal-fixed_region check (matches v1).
            for (std::size_t i = 0; i < NF; ++i)
            {
               if (is_var[i])
               {
                  if (offsets[i] != fixed_region)
                     return codec_fail(
                        "ssz: record first variable offset != fixed_region",
                        static_cast<std::uint32_t>(pos), "ssz");
                  break;
               }
            }

            codec_status err = codec_ok();
            (
               ([&]
                {
                   if (!err.ok()) return;
                   using F = typename R::template member_type<Is>;
                   if constexpr (is_fixed_v<F>) return;
                   const std::size_t beg = pos + offsets[Is];
                   const std::size_t fin = pos + var_end[Is];
                   auto st = validate_value<F>(src, beg, fin);
                   if (!st.ok()) err = std::move(st);
                }()),
               ...);
            return err;
         }
      }

      // ── Bitlist helper (SSZ Bitlist[N]) ───────────────────────────────────

      template <std::size_t MaxN>
      codec_status validate_bitlist_payload(std::span<const char> src,
                                            std::size_t            pos,
                                            std::size_t            end) noexcept
      {
         if (pos >= end || end > src.size())
            return codec_fail("ssz: bitlist span out of bounds",
                              static_cast<std::uint32_t>(pos), "ssz");
         // Find the highest set bit in the span — that's the delimiter
         // bit; bit_count = position of that bit. Reject empty span
         // (no delimiter) and over-length spans (bit_count > MaxN).
         std::size_t span = end - pos;
         std::size_t last = span;
         while (last > 0 &&
                static_cast<std::uint8_t>(src[pos + last - 1]) == 0)
            --last;
         if (last == 0)
            return codec_fail("ssz: bitlist missing delimiter",
                              static_cast<std::uint32_t>(pos), "ssz");
         std::uint8_t lb = static_cast<std::uint8_t>(src[pos + last - 1]);
         int hi = 31 - __builtin_clz(static_cast<unsigned int>(lb));
         std::size_t bits =
            (last - 1) * 8 + static_cast<std::size_t>(hi);
         if (bits > MaxN)
            return codec_fail("ssz: bitlist bit_count exceeds bound",
                              static_cast<std::uint32_t>(pos), "ssz");
         return codec_ok();
      }

      // ── Throwing walker (native, exceptions enabled) ──────────────────────
      //
      // Mirrors validate_value<T> shape-for-shape but raises
      // codec_exception on failure and returns void on success. The
      // [[noreturn, gnu::cold]] failure helper lets the optimizer keep
      // the "no throw reachable" success path on the hot icache line
      // and elide the rest, matching v1's success-path cost on shapes
      // where range-propagation can prove all checks pass.

      [[noreturn, gnu::cold]] inline void
      ssz_throw_fail(std::string_view msg, std::uint32_t off)
      {
         throw codec_exception{codec_error{msg, off, "ssz"}};
      }

      template <typename T>
      void validate_or_throw_value(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end);

      template <typename E>
      void validate_or_throw_vector_payload(std::span<const char> src,
                                            std::size_t            pos,
                                            std::size_t            end)
      {
         if (pos > end || end > src.size()) [[unlikely]]
            ssz_throw_fail("ssz: vector span out of bounds",
                           static_cast<std::uint32_t>(pos));
         if (pos == end) return;

         if constexpr (is_fixed_v<E>)
         {
            const std::size_t esz = fixed_size_of<E>();
            if ((end - pos) % esz != 0) [[unlikely]]
               ssz_throw_fail("ssz: vector span not a multiple of "
                              "element size",
                              static_cast<std::uint32_t>(pos));
            constexpr bool memcpy_layout =
               Record<E> && std::is_trivially_copyable_v<E> &&
               fixed_size_of<E>() == sizeof(E);
            if constexpr (Record<E> && !memcpy_layout)
            {
               const std::size_t n = (end - pos) / esz;
               for (std::size_t i = 0; i < n; ++i)
                  validate_or_throw_value<E>(src, pos + i * esz,
                                              pos + (i + 1) * esz);
            }
         }
         else
         {
            if (end - pos < 4) [[unlikely]]
               ssz_throw_fail("ssz: variable list missing offset table",
                              static_cast<std::uint32_t>(pos));
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            if (first % 4 != 0 || first > span) [[unlikely]]
               ssz_throw_fail("ssz: invalid first list offset",
                              static_cast<std::uint32_t>(pos));
            const std::size_t n = first / 4;
            if (span < n * 4) [[unlikely]]
               ssz_throw_fail("ssz: list offset table truncated",
                              static_cast<std::uint32_t>(pos));
            std::uint32_t prev = first;
            for (std::size_t i = 1; i < n; ++i)
            {
               std::uint32_t off_i = 0;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (off_i < prev || off_i > span) [[unlikely]]
                  ssz_throw_fail("ssz: list offset out of range",
                                 static_cast<std::uint32_t>(pos + i * 4));
               prev = off_i;
            }
            for (std::size_t i = 0; i < n; ++i)
            {
               std::uint32_t off_i = 0, stop = span;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (i + 1 < n)
                  std::memcpy(&stop, src.data() + pos + (i + 1) * 4, 4);
               validate_or_throw_value<E>(src, pos + off_i, pos + stop);
            }
         }
      }

      template <typename E, std::size_t N>
      void validate_or_throw_array_payload(std::span<const char> src,
                                           std::size_t            pos,
                                           std::size_t            end)
      {
         if constexpr (is_fixed_v<E>)
         {
            constexpr std::size_t esz = fixed_size_of<E>();
            if (end - pos < N * esz) [[unlikely]]
               ssz_throw_fail("ssz: array buffer too small",
                              static_cast<std::uint32_t>(pos));
            constexpr bool memcpy_layout =
               Record<E> && std::is_trivially_copyable_v<E> &&
               fixed_size_of<E>() == sizeof(E);
            if constexpr (Record<E> && !memcpy_layout)
            {
               for (std::size_t i = 0; i < N; ++i)
                  validate_or_throw_value<E>(src, pos + i * esz,
                                              pos + (i + 1) * esz);
            }
         }
         else
         {
            if constexpr (N == 0) return;
            if (end - pos < N * 4) [[unlikely]]
               ssz_throw_fail("ssz: array offset table truncated",
                              static_cast<std::uint32_t>(pos));
            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            if (first != N * 4 || first > span) [[unlikely]]
               ssz_throw_fail("ssz: invalid array first offset",
                              static_cast<std::uint32_t>(pos));
            std::uint32_t prev = first;
            for (std::size_t i = 1; i < N; ++i)
            {
               std::uint32_t off_i = 0;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (off_i < prev || off_i > span) [[unlikely]]
                  ssz_throw_fail("ssz: array offset out of range",
                                 static_cast<std::uint32_t>(pos + i * 4));
               prev = off_i;
            }
            for (std::size_t i = 0; i < N; ++i)
            {
               std::uint32_t off_i = 0, stop = span;
               std::memcpy(&off_i, src.data() + pos + i * 4, 4);
               if (i + 1 < N)
                  std::memcpy(&stop, src.data() + pos + (i + 1) * 4, 4);
               validate_or_throw_value<E>(src, pos + off_i, pos + stop);
            }
         }
      }

      template <std::size_t MaxN>
      void validate_or_throw_bitlist_payload(std::span<const char> src,
                                             std::size_t            pos,
                                             std::size_t            end)
      {
         if (pos >= end || end > src.size()) [[unlikely]]
            ssz_throw_fail("ssz: bitlist span out of bounds",
                           static_cast<std::uint32_t>(pos));
         std::size_t span = end - pos;
         std::size_t last = span;
         while (last > 0 &&
                static_cast<std::uint8_t>(src[pos + last - 1]) == 0)
            --last;
         if (last == 0) [[unlikely]]
            ssz_throw_fail("ssz: bitlist missing delimiter",
                           static_cast<std::uint32_t>(pos));
         std::uint8_t lb = static_cast<std::uint8_t>(src[pos + last - 1]);
         int hi = 31 - __builtin_clz(static_cast<unsigned int>(lb));
         std::size_t bits =
            (last - 1) * 8 + static_cast<std::size_t>(hi);
         if (bits > MaxN) [[unlikely]]
            ssz_throw_fail("ssz: bitlist bit_count exceeds bound",
                           static_cast<std::uint32_t>(pos));
      }

      template <Record T, std::size_t... Is>
      void validate_or_throw_record(std::span<const char> src,
                                    std::size_t            pos,
                                    std::size_t            end,
                                    std::index_sequence<Is...>)
      {
         using R = ::psio::reflect<T>;
         constexpr std::size_t NF = R::member_count;

         constexpr std::size_t fixed_region = (
            ssz_member_fixed_size_simple<T, Is>() + ... + std::size_t{0});

         if (end - pos < fixed_region) [[unlikely]]
            ssz_throw_fail("ssz: record fixed region truncated",
                           static_cast<std::uint32_t>(pos));

         if constexpr (is_fixed_v<T>)
         {
            // Fully-fixed Record: every field is a fixed primitive,
            // fixed array, fixed bitvector, or another fixed Record.
            // The outer `end - pos >= fixed_region` check already
            // proves the buffer fits every field's bytes. Per-field
            // recursion would just re-check spans the compiler can
            // already prove via algebra — but in practice GCC
            // doesn't propagate the bound across the lambda fold for
            // 9-field records, so each redundant check survives as a
            // ~1-cycle compare. Skip the recursion entirely.
            return;
         }
         else
         {
            std::array<std::uint32_t, NF> offsets{};
            std::array<bool, NF>          is_var{};
            std::size_t                   fp = pos;
            (
               ([&]
                {
                   using F = typename R::template member_type<Is>;
                   if constexpr (is_fixed_v<F>)
                   {
                      is_var[Is] = false;
                      fp        += fixed_size_of<F>();
                   }
                   else
                   {
                      is_var[Is] = true;
                      std::uint32_t o = 0;
                      std::memcpy(&o, src.data() + fp, 4);
                      offsets[Is] = o;
                      fp         += 4;
                   }
                }()),
               ...);

            const std::uint32_t span =
               static_cast<std::uint32_t>(end - pos);
            std::uint32_t prev = static_cast<std::uint32_t>(fixed_region);
            for (std::size_t i = 0; i < NF; ++i)
            {
               if (!is_var[i]) continue;
               if (offsets[i] < prev || offsets[i] > span) [[unlikely]]
                  ssz_throw_fail("ssz: record variable offset out of range",
                                 static_cast<std::uint32_t>(pos));
               prev = offsets[i];
            }

            std::array<std::uint32_t, NF> var_end{};
            {
               std::uint32_t last_end = span;
               for (std::size_t i = NF; i-- > 0;)
               {
                  if (is_var[i])
                  {
                     var_end[i] = last_end;
                     last_end   = offsets[i];
                  }
               }
            }

            for (std::size_t i = 0; i < NF; ++i)
            {
               if (is_var[i])
               {
                  if (offsets[i] != fixed_region) [[unlikely]]
                     ssz_throw_fail(
                        "ssz: record first variable offset != fixed_region",
                        static_cast<std::uint32_t>(pos));
                  break;
               }
            }

            (
               ([&]
                {
                   using F = typename R::template member_type<Is>;
                   if constexpr (!is_fixed_v<F>)
                   {
                      const std::size_t beg = pos + offsets[Is];
                      const std::size_t fin = pos + var_end[Is];
                      validate_or_throw_value<F>(src, beg, fin);
                   }
                }()),
               ...);
         }
      }

      template <typename T>
      void validate_or_throw_value(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::ssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            auto st = Proj::validate(std::span<const char>(
               src.data() + pos,
               (end > pos) ? (end - pos) : 0));
            if (!st.ok()) [[unlikely]]
               throw codec_exception{st.error()};
         }
         else if constexpr (is_fixed_v<T> && !Record<T>)
         {
            if (end - pos < fixed_size_of<T>()) [[unlikely]]
               ssz_throw_fail("ssz: buffer too small for fixed primitive",
                              static_cast<std::uint32_t>(pos));
            if constexpr (is_std_array_v<T>)
            {
               using E = typename T::value_type;
               if constexpr (Record<E>)
               {
                  constexpr std::size_t N   = std::tuple_size<T>::value;
                  constexpr std::size_t esz = fixed_size_of<E>();
                  for (std::size_t i = 0; i < N; ++i)
                     validate_or_throw_value<E>(src, pos + i * esz,
                                                 pos + (i + 1) * esz);
               }
            }
         }
         else if constexpr (is_bitlist_v<T>)
            validate_or_throw_bitlist_payload<T::max_size_value>(
               src, pos, end);
         else if constexpr (std::is_same_v<T, std::string>)
         {
            if (pos > end || end > src.size()) [[unlikely]]
               ssz_throw_fail("ssz: string span out of bounds",
                              static_cast<std::uint32_t>(pos));
         }
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            validate_or_throw_array_payload<E, N>(src, pos, end);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            validate_or_throw_vector_payload<E>(src, pos, end);
         }
         else if constexpr (is_std_optional_v<T>)
         {
            if (pos > end || end > src.size()) [[unlikely]]
               ssz_throw_fail("ssz: optional span out of bounds",
                              static_cast<std::uint32_t>(pos));
            if (pos == end) [[unlikely]]
               ssz_throw_fail("ssz: optional missing selector",
                              static_cast<std::uint32_t>(pos));
            const std::uint8_t sel = static_cast<std::uint8_t>(src[pos]);
            if (sel == 0)
            {
               if (end - pos != 1) [[unlikely]]
                  ssz_throw_fail("ssz: None optional has trailing bytes",
                                 static_cast<std::uint32_t>(pos));
               return;
            }
            if (sel != 1) [[unlikely]]
               ssz_throw_fail("ssz: optional selector not 0/1",
                              static_cast<std::uint32_t>(pos));
            using V = typename T::value_type;
            validate_or_throw_value<V>(src, pos + 1, end);
         }
         else if constexpr (is_std_variant_v<T>)
         {
            if (pos >= end || end > src.size()) [[unlikely]]
               ssz_throw_fail("ssz: variant span out of bounds",
                              static_cast<std::uint32_t>(pos));
            const std::uint8_t sel = static_cast<std::uint8_t>(src[pos]);
            constexpr std::size_t NA = std::variant_size_v<T>;
            if (sel >= NA) [[unlikely]]
               ssz_throw_fail("ssz: variant selector out of range",
                              static_cast<std::uint32_t>(pos));
            [&]<std::size_t... Js>(std::index_sequence<Js...>) {
               (((sel == Js)
                    ? (validate_or_throw_value<
                          std::variant_alternative_t<Js, T>>(
                          src, pos + 1, end),
                       true)
                    : false) ||
                ...);
            }(std::make_index_sequence<NA>{});
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            validate_or_throw_record<T>(
               src, pos, end, std::make_index_sequence<R::member_count>{});
         }
         else
         {
            ssz_throw_fail("ssz: unsupported type in validate",
                           static_cast<std::uint32_t>(pos));
         }
      }

      // ── Main dispatch ─────────────────────────────────────────────────────

      template <typename T>
      codec_status validate_value(std::span<const char> src,
                                  std::size_t            pos,
                                  std::size_t            end) noexcept
      {
         // Adapter dispatch: opaque payload — defer to the adapter's
         // own validator.
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::ssz, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            return Proj::validate(std::span<const char>(
               src.data() + pos,
               (end > pos) ? (end - pos) : 0));
         }
         else if constexpr (is_fixed_v<T> && !Record<T>)
         {
            // Fixed primitive / bitvector / fixed std::array.
            if (end - pos < fixed_size_of<T>())
               return codec_fail("ssz: buffer too small for fixed primitive",
                                 static_cast<std::uint32_t>(pos), "ssz");
            // is_std_array path needs per-element recursion when the
            // element is a Record (non-bitwise).
            if constexpr (is_std_array_v<T>)
            {
               using E = typename T::value_type;
               if constexpr (Record<E>)
               {
                  constexpr std::size_t N   = std::tuple_size<T>::value;
                  constexpr std::size_t esz = fixed_size_of<E>();
                  for (std::size_t i = 0; i < N; ++i)
                  {
                     auto st = validate_value<E>(src, pos + i * esz,
                                                 pos + (i + 1) * esz);
                     if (!st.ok()) return st;
                  }
               }
            }
            return codec_ok();
         }
         else if constexpr (is_bitlist_v<T>)
            return validate_bitlist_payload<T::max_size_value>(src, pos, end);
         else if constexpr (std::is_same_v<T, std::string>)
         {
            if (pos > end || end > src.size())
               return codec_fail("ssz: string span out of bounds",
                                 static_cast<std::uint32_t>(pos), "ssz");
            return codec_ok();
         }
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            return validate_array_payload<E, N>(src, pos, end);
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            return validate_vector_payload<E>(src, pos, end);
         }
         else if constexpr (is_std_optional_v<T>)
         {
            // SSZ Union[null, T]: 0x00 | 0x01 + payload.
            if (pos > end || end > src.size())
               return codec_fail("ssz: optional span out of bounds",
                                 static_cast<std::uint32_t>(pos), "ssz");
            if (pos == end)
               return codec_fail("ssz: optional missing selector",
                                 static_cast<std::uint32_t>(pos), "ssz");
            const std::uint8_t sel =
               static_cast<std::uint8_t>(src[pos]);
            if (sel == 0)
            {
               if (end - pos != 1)
                  return codec_fail("ssz: None optional has trailing bytes",
                                    static_cast<std::uint32_t>(pos), "ssz");
               return codec_ok();
            }
            if (sel != 1)
               return codec_fail("ssz: optional selector not 0/1",
                                 static_cast<std::uint32_t>(pos), "ssz");
            using V = typename T::value_type;
            return validate_value<V>(src, pos + 1, end);
         }
         else if constexpr (is_std_variant_v<T>)
         {
            // SSZ Union: 1-byte selector + alt.
            if (pos >= end || end > src.size())
               return codec_fail("ssz: variant span out of bounds",
                                 static_cast<std::uint32_t>(pos), "ssz");
            const std::uint8_t sel =
               static_cast<std::uint8_t>(src[pos]);
            constexpr std::size_t NA = std::variant_size_v<T>;
            if (sel >= NA)
               return codec_fail("ssz: variant selector out of range",
                                 static_cast<std::uint32_t>(pos), "ssz");
            return [&]<std::size_t... Js>(std::index_sequence<Js...>) noexcept {
               codec_status err = codec_ok();
               (((sel == Js)
                    ? (err = validate_value<
                                std::variant_alternative_t<Js, T>>(
                          src, pos + 1, end),
                       true)
                    : false) ||
                ...);
               return err;
            }(std::make_index_sequence<NA>{});
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            return validate_record<T>(
               src, pos, end, std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return codec_fail("ssz: unsupported type in validate",
                              static_cast<std::uint32_t>(pos), "ssz");
         }
      }

   }  // namespace detail::ssz_impl

   // ── Format tag ─────────────────────────────────────────────────────────

   struct ssz : format_tag_base<ssz>
   {
      // Identifier for validate() error messages / tool routing.
      static constexpr const char* name = "ssz";

      using preferred_presentation_category = ::psio::binary_category;

      // ── encode ─────────────────────────────────────────────────────────
      //
      // Two-pass: size_of_v computes the byte total (O(variable-field
      // count) for containers, O(1) for fixed types), then a single
      // resize + fast_buf_stream walk writes the bytes. Mirrors v1's
      // `convert_to_ssz` algorithm — eliminates the per-record resize
      // + zero-init that the prior single-pass vector_stream walker
      // accumulated on nested mixed records.
      template <typename T>
      friend void tag_invoke(decltype(::psio::encode),
                             ssz,
                             const T&              v,
                             std::vector<char>&    sink)
      {
         std::size_t total;
         if constexpr (detail::ssz_impl::is_fixed_v<T>)
            total = detail::ssz_impl::fixed_size_of<T>();
         else
            total = detail::ssz_impl::size_of_v(v);
         const std::size_t orig = sink.size();
         sink.resize(orig + total);
         ::psio::fast_buf_stream fbs(sink.data() + orig, total);
         detail::ssz_impl::encode_value(v, fbs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          ssz,
                                          const T& v)
      {
         std::size_t total;
         if constexpr (detail::ssz_impl::is_fixed_v<T>)
            total = detail::ssz_impl::fixed_size_of<T>();
         else
            total = detail::ssz_impl::size_of_v(v);
         std::vector<char> out(total);
         ::psio::fast_buf_stream fbs(out.data(), total);
         detail::ssz_impl::encode_value(v, fbs);
         return out;
      }

      // ── decode ─────────────────────────────────────────────────────────
      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>),
                          ssz,
                          T*,
                          std::span<const char> bytes)
      {
         return detail::ssz_impl::decode_value<T>(bytes, 0, bytes.size());
      }

      // ── size_of ────────────────────────────────────────────────────────
      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of),
                                    ssz,
                                    const T& v)
      {
         if constexpr (detail::ssz_impl::is_fixed_v<T>)
            return detail::ssz_impl::fixed_size_of<T>();
         else
            return detail::ssz_impl::size_of_v(v);
      }

      // ── validate (structural only, no-throw) ───────────────────────────
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     ssz,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "ssz");
             !st.ok())
            return st;
         return detail::ssz_impl::validate_value<T>(bytes, 0, bytes.size());
      }

      // ── validate_or_throw (structural only, throwing) ──────────────────
      //
      // Native-only: throws codec_exception on first failure, void on
      // success. Compiler can elide the entire check chain when it
      // proves no throw is reachable, matching v1's success-path cost.
      template <typename T>
      friend void tag_invoke(decltype(::psio::validate_or_throw<T>),
                             ssz, T*, std::span<const char> bytes)
      {
         ::psio::enforce_max_dynamic_cap<T>(bytes.size(), "ssz");
         detail::ssz_impl::validate_or_throw_value<T>(
            bytes, 0, bytes.size());
      }

      // ── validate_strict — structural + spec-carried semantic checks ──
      //
      // Design §5.3.3 / §5.4: structural validate first, then walk each
      // reflected field invoking any `static codec_status validate(span)`
      // members on the field's effective annotations. First failure
      // wins. Spec set is open — third-party spec types in user code
      // are picked up by the SFINAE-driven walker.
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     ssz,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         auto st =
            detail::ssz_impl::validate_value<T>(bytes, 0, bytes.size());
         if (!st.ok())
            return st;
         if constexpr (::psio::Reflected<T>)
         {
            try
            {
               T decoded = detail::ssz_impl::decode_value<T>(bytes, 0,
                                                              bytes.size());
               return ::psio::validate_specs_on_value(decoded);
            }
            catch (...)
            {
               return codec_fail(
                  "ssz: decode failed during validate_strict", 0, "ssz");
            }
         }
         else
         {
            return st;
         }
      }

      // ── make_boxed — default decode + std::make_unique ─────────────────
      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           ssz,
                                           T*,
                                           std::span<const char> bytes) noexcept
      {
         return std::make_unique<T>(
            detail::ssz_impl::decode_value<T>(bytes, 0, bytes.size()));
      }
   };

}  // namespace psio
