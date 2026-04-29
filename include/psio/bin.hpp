#pragma once
//
// psio/bin.hpp — `bin` format tag.
//
// `bin` is the simplest binary format: primitives serialize as raw LE
// bytes; std::string and std::vector get a u32 length prefix; std::array
// and reflected records concatenate their elements. No offset tables,
// no headers, no heap region.
//
// Scope (Phase 10 MVP): primitives, std::array, std::vector, std::string,
// std::optional, reflected records. Matches the other-format MVPs.

#include <psio/cpo.hpp>
#include <psio/detail/variant_util.hpp>
#include <psio/error.hpp>
#include <psio/ext_int.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/validate_strict_walker.hpp>
#include <psio/varint/leb128.hpp>
#include <psio/wrappers.hpp>  // effective_annotations_for

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

   struct bin;  // fwd — the dispatch trait below needs the complete
                // format-tag identity, but only at instantiation time.

   namespace detail::bin_impl {

      template <typename T>
      concept Record = ::psio::Reflected<T>;

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
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio::bitvector<N>> : std::true_type {};

      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio::bitlist<N>> : std::true_type {};

      inline std::uint32_t read_u32(std::span<const char> src, std::size_t pos)
      {
         std::uint32_t v{};
         std::memcpy(&v, src.data() + pos, 4);
         return v;
      }

      // ── varuint32 ────────────────────────────────────────────────────────
      //
      // The EOSIO bin wire format uses LEB128-style varuint32 everywhere a
      // length / count / index is emitted: string size, vector size,
      // variant index, bitlist bit count, record content_size prefix.
      // The wire encoding (7-bit groups, MSB = continuation, max 5 bytes
      // for a full u32) is implemented in psio/varint/leb128.hpp; the
      // wrappers below bridge that header-only library to the bin
      // codec's `Sink::write(const char*, n)` and `std::span<const char>`
      // calling conventions.  Decoders that care about strict canonicity
      // (e.g. validate) should use `varint::leb128::scalar::decode_u32`
      // directly and check the `ok` field; the wrapper here mirrors the
      // prior best-effort behaviour (returns 0 on malformed input).

      constexpr std::size_t varuint32_size(std::uint32_t v) noexcept
      {
         return ::psio::varint::leb128::size_u32(v);
      }

      template <typename Sink>
      void write_varuint32(Sink& s, std::uint32_t v) noexcept
      {
         std::uint8_t buf[::psio::varint::leb128::max_bytes_u32];
         const auto   n = ::psio::varint::leb128::encode_u32(buf, v);
         s.write(reinterpret_cast<const char*>(buf), n);
      }

      inline std::uint32_t
      read_varuint32(std::span<const char> src, std::size_t& pos) noexcept
      {
         const auto avail = src.size() - pos;
         const auto r     = ::psio::varint::leb128::decode_u32(
            reinterpret_cast<const std::uint8_t*>(src.data() + pos), avail);
         if (!r.ok) return 0;
         pos += r.len;
         return r.value;
      }

      // Sinks that only count bytes (psio::size_stream) skip the actual
      // adapter encode in the dispatch path and just advance by the
      // adapter's packsize. Other sinks encode into a scratch vector
      // and copy the bytes through.
      template <typename S>
      struct sink_counts_only : std::false_type {};
      template <>
      struct sink_counts_only<::psio::size_stream> : std::true_type {};
      template <typename S>
      inline constexpr bool sink_counts_only_v = sink_counts_only<S>::value;

      // ── Fixed / variable packsize split ─────────────────────────────────
      //
      // `fixed_contrib<T>()` is the minimum byte count T contributes
      // regardless of runtime content — all compile-time constant. For
      // types with no variable-length parts it's the entire packsize.
      //
      // `variable_contrib(v)` reads only what it must from `v`: string
      // sizes, vector sizes (and their payloads when the element type is
      // variable), optional discriminants, variant active-alt content.
      // Types that are fully fixed produce zero variable contribution
      // and the function compiles down to `return 0;` at every call
      // site.
      //
      // `packed_size_of(v) = fixed_contrib<T>() + variable_contrib(v)`.
      // This avoids the naive walker's per-field memory traversal for
      // deeply nested records dominated by fixed-size leaf types.

      template <typename T>
      consteval std::size_t fixed_contrib();

      template <typename T>
      std::size_t variable_contrib(const T& v);

      template <typename T>
      std::size_t record_body_size(const T& v);

      template <typename T>
      consteval bool fully_fixed();

      template <typename T>
      consteval bool fully_fixed()
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::bin, T>)
            return false;  // adapter payload is runtime-sized
         else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                            std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128> ||
                            std::is_same_v<T, ::psio::uint256>)
            return true;
         else if constexpr (is_std_array<T>::value)
            return fully_fixed<typename T::value_type>();
         else if constexpr (is_bitvector<T>::value)
            return true;
         else if constexpr (std::is_same_v<T, std::string> ||
                            is_std_vector<T>::value ||
                            is_std_optional<T>::value ||
                            is_std_variant<T>::value ||
                            is_bitlist<T>::value)
            return false;
         else if constexpr (Record<T>)
         {
            // Non-DWNC records carry a varuint content_size prefix
            // (runtime-sized) so they are not fully fixed regardless
            // of field types. Only DWNC all-fixed records are.
            if constexpr (!::psio::is_dwnc_v<T>)
               return false;
            using R  = ::psio::reflect<T>;
            bool all = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((all = all && fully_fixed<
                                  typename R::template member_type<Is>>()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return all;
         }
         else
            return false;
      }

      template <typename T>
      consteval std::size_t fixed_contrib()
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::bin, T>)
            return 0;  // varuint length prefix + variable payload
         else if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            return 16;
         else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            return sizeof(T);
         else if constexpr (is_std_array<T>::value)
            return std::tuple_size<T>::value *
                   fixed_contrib<typename T::value_type>();
         else if constexpr (is_bitvector<T>::value)
            return (T::size_value + 7) / 8;
         else if constexpr (std::is_same_v<T, std::string>)
            return 0;  // varuint length — contributed at runtime
         else if constexpr (is_std_vector<T>::value)
            return 0;  // varuint length — contributed at runtime
         else if constexpr (is_std_optional<T>::value)
            return 1;
         else if constexpr (is_std_variant<T>::value)
            return 0;  // varuint index — contributed at runtime
         else if constexpr (is_bitlist<T>::value)
            return 0;  // varuint bit_count — contributed at runtime
         else if constexpr (Record<T>)
         {
            // DWNC all-fixed records sum their children; everything else
            // (non-DWNC records, records with variable fields) is
            // accounted for at runtime in variable_contrib.
            if constexpr (!::psio::is_dwnc_v<T>)
               return 0;
            std::size_t total = 0;
            using R           = ::psio::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((total += fixed_contrib<
                             typename R::template member_type<Is>>()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return total;
         }
         else
            return 0;
      }

      template <typename T>
      std::size_t variable_contrib(const T& v)
      {
         if constexpr (fully_fixed<T>())
            return 0;
         else if constexpr (::psio::format_should_dispatch_adapter_v<
                               ::psio::bin, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            const auto n = Proj::packsize(v);
            return varuint32_size(static_cast<std::uint32_t>(n)) + n;
         }
         else if constexpr (std::is_same_v<T, std::string>)
            return varuint32_size(static_cast<std::uint32_t>(v.size())) +
                   v.size();
         else if constexpr (is_std_vector<T>::value)
         {
            using E           = typename T::value_type;
            std::size_t total = varuint32_size(
               static_cast<std::uint32_t>(v.size()));
            if constexpr (fully_fixed<E>())
               total += v.size() * fixed_contrib<E>();
            else
            {
               for (const auto& x : v)
                  total += fixed_contrib<E>() + variable_contrib(x);
            }
            return total;
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            // Fully-fixed array short-circuits via the top guard.
            std::size_t total = 0;
            for (const auto& x : v)
               total += variable_contrib(x);
            return total;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (!v.has_value())
               return 0;
            using E = typename T::value_type;
            return fixed_contrib<E>() + variable_contrib(*v);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            std::size_t total = 0;
            std::visit(
               [&](const auto& alt)
               {
                  using A = std::remove_cvref_t<decltype(alt)>;
                  total   = varuint32_size(
                              static_cast<std::uint32_t>(v.index())) +
                          fixed_contrib<A>() + variable_contrib(alt);
               },
               v);
            return total;
         }
         else if constexpr (is_bitlist<T>::value)
            return varuint32_size(
                      static_cast<std::uint32_t>(v.size())) +
                   v.bytes().size();
         else if constexpr (Record<T>)
         {
            // Two-step: compute body, wrap with varuint prefix when
            // non-DWNC. record_body_size is shared with the encode
            // path so the size walk happens exactly once per
            // non-DWNC record.
            const std::size_t body = record_body_size(v);
            if constexpr (!::psio::is_dwnc_v<T>)
               return varuint32_size(static_cast<std::uint32_t>(body)) + body;
            return body;
         }
         else
            return 0;
      }

      // Body size of a reflected record — sum of per-field wire bytes,
      // no varuint content_size prefix. The caller adds the prefix for
      // non-DWNC records. Shared between variable_contrib<Record> and
      // the encode path so the size walk happens once.
      template <typename T>
      std::size_t record_body_size(const T& v)
      {
         using R           = ::psio::reflect<T>;
         std::size_t body  = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
               ([&]
                {
                   using F = typename R::template member_type<Is>;
                   body += fixed_contrib<F>();
                   if constexpr (!fully_fixed<F>())
                      body += variable_contrib(
                         v.*(R::template member_pointer<Is>));
                }()),
               ...);
         }(std::make_index_sequence<R::member_count>{});
         return body;
      }

      template <typename T>
      std::size_t packed_size_of(const T& v)
      {
         return fixed_contrib<T>() + variable_contrib(v);
      }

      // ── Stack-cached size precomputation ────────────────────────
      //
      // The naive write_value path calls record_body_size(v) for
      // every non-DWNC nested record (line ~560 below) just to emit
      // the varuint length prefix.  Each call recurses, and each
      // outer level above does its own recursion → fractal O(D × N)
      // cost on deeply nested data.
      //
      // The cache pipeline mirrors what protobuf does: walk once
      // during a size-collect pass, populate sizes[idx++] with each
      // non-DWNC record's body size, then walk again during write
      // reading sizes[idx++] instead of recomputing.  Total work
      // becomes O(N) regardless of nesting depth.

      template <typename T>
      std::size_t variable_contrib_collect(const T& v,
                                           std::uint32_t* sizes,
                                           std::size_t&   idx);

      template <typename T>
      std::size_t record_body_collect(const T&       v,
                                      std::uint32_t* sizes,
                                      std::size_t&   idx)
      {
         using R           = ::psio::reflect<T>;
         std::size_t body  = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
               ([&]
                {
                   using F = typename R::template member_type<Is>;
                   body += fixed_contrib<F>();
                   if constexpr (!fully_fixed<F>())
                      body += variable_contrib_collect(
                         v.*(R::template member_pointer<Is>), sizes,
                         idx);
                }()),
               ...);
         }(std::make_index_sequence<R::member_count>{});
         return body;
      }

      template <typename T>
      std::size_t variable_contrib_collect(const T&       v,
                                           std::uint32_t* sizes,
                                           std::size_t&   idx)
      {
         if constexpr (fully_fixed<T>())
            return 0;
         else if constexpr (::psio::format_should_dispatch_adapter_v<
                               ::psio::bin, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            const auto n = Proj::packsize(v);
            return varuint32_size(static_cast<std::uint32_t>(n)) + n;
         }
         else if constexpr (std::is_same_v<T, std::string>)
            return varuint32_size(static_cast<std::uint32_t>(v.size())) +
                   v.size();
         else if constexpr (is_std_vector<T>::value)
         {
            using E           = typename T::value_type;
            std::size_t total = varuint32_size(
               static_cast<std::uint32_t>(v.size()));
            if constexpr (fully_fixed<E>())
               total += v.size() * fixed_contrib<E>();
            else
            {
               for (const auto& x : v)
                  total += fixed_contrib<E>() +
                           variable_contrib_collect(x, sizes, idx);
            }
            return total;
         }
         else if constexpr (is_std_array<T>::value)
         {
            std::size_t total = 0;
            for (const auto& x : v)
               total += variable_contrib_collect(x, sizes, idx);
            return total;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (!v.has_value())
               return 0;
            using E = typename T::value_type;
            return fixed_contrib<E>() +
                   variable_contrib_collect(*v, sizes, idx);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            std::size_t total = 0;
            std::visit(
               [&](const auto& alt)
               {
                  using A = std::remove_cvref_t<decltype(alt)>;
                  total   = varuint32_size(
                              static_cast<std::uint32_t>(v.index())) +
                          fixed_contrib<A>() +
                          variable_contrib_collect(alt, sizes, idx);
               },
               v);
            return total;
         }
         else if constexpr (is_bitlist<T>::value)
            return varuint32_size(
                      static_cast<std::uint32_t>(v.size())) +
                   v.bytes().size();
         else if constexpr (Record<T>)
         {
            if constexpr (!::psio::is_dwnc_v<T>)
            {
               //  Reserve a slot, recurse to populate inner slots,
               //  then patch the reserved slot with this record's
               //  body size.
               const std::size_t body_idx = idx++;
               const std::size_t body =
                  record_body_collect(v, sizes, idx);
               sizes[body_idx] = static_cast<std::uint32_t>(body);
               return varuint32_size(
                         static_cast<std::uint32_t>(body)) +
                      body;
            }
            else
               return record_body_collect(v, sizes, idx);
         }
         else
            return 0;
      }

      template <typename T>
      std::size_t packed_size_of_collect(const T&       v,
                                         std::uint32_t* sizes,
                                         std::size_t&   idx)
      {
         return fixed_contrib<T>() +
                variable_contrib_collect(v, sizes, idx);
      }

      // Count of non-DWNC reflected records inside v — drives the
      // sizes[] allocation for the cache pipeline.
      template <typename T>
      std::size_t bin_count_records(const T& v) noexcept
      {
         if constexpr (fully_fixed<T>())
            return 0;
         else if constexpr (::psio::format_should_dispatch_adapter_v<
                               ::psio::bin, T>)
            return 0;  // adapter encodes opaquely, no slots
         else if constexpr (std::is_same_v<T, std::string>)
            return 0;
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (fully_fixed<E>())
               return 0;
            std::size_t k = 0;
            for (const auto& x : v)
               k += bin_count_records(x);
            return k;
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (fully_fixed<E>())
               return 0;
            std::size_t k = 0;
            for (const auto& x : v)
               k += bin_count_records(x);
            return k;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (!v.has_value())
               return 0;
            return bin_count_records(*v);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            std::size_t k = 0;
            std::visit(
               [&](const auto& alt) { k = bin_count_records(alt); }, v);
            return k;
         }
         else if constexpr (is_bitlist<T>::value)
            return 0;
         else if constexpr (Record<T>)
         {
            using R       = ::psio::reflect<T>;
            std::size_t k = ::psio::is_dwnc_v<T> ? std::size_t{0}
                                                  : std::size_t{1};
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      if constexpr (!fully_fixed<F>())
                         k += bin_count_records(
                            v.*(R::template member_pointer<Is>));
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return k;
         }
         else
            return 0;
      }

      // ── Unified walker ───────────────────────────────────────────────────
      //
      // One template handles both packsize (Sink = psio::size_stream) and
      // encode (Sink = psio::fast_buf_stream / vector_stream). The sink
      // concept is uniform: `.put<T>(T)` writes sizeof(T) bytes,
      // `.write(ptr, n)` writes n bytes, `.write(ch)` writes a single
      // byte. `size_stream` specializations of those just sum sizes.

      //  Optional `sizes` / `idx_ptr` carry per-record body sizes
      //  precomputed by packed_size_of_collect, eliminating the
      //  fractal record_body_size() call at every nested non-DWNC
      //  record.  When nullptr (or no caller passes them), the path
      //  falls back to the runtime walk.
      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s,
                       const std::uint32_t* sizes   = nullptr,
                       std::size_t*         idx_ptr = nullptr)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::bin, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                          ::psio::binary_category>;
            if constexpr (sink_counts_only_v<Sink>)
            {
               const auto n = Proj::packsize(v);
               s.write(nullptr,
                       varuint32_size(static_cast<std::uint32_t>(n)) + n);
            }
            else
            {
               std::vector<char> tmp;
               Proj::encode(v, tmp);
               write_varuint32(s, static_cast<std::uint32_t>(tmp.size()));
               s.write(tmp.data(), tmp.size());
            }
            return;
         }

         if constexpr (std::is_same_v<T, bool>)
         {
            std::uint8_t b = v ? 1 : 0;
            s.write(reinterpret_cast<const char*>(&b), 1);
         }
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
            s.write(v.limb, 32);
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
            s.write(&v, 16);
         else if constexpr (std::is_arithmetic_v<T>)
            s.put(v);
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (std::is_arithmetic_v<E>)
               s.write(v.data(), v.size() * sizeof(E));
            else
               for (const auto& x : v)
                  write_value(x, s, sizes, idx_ptr);
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            write_varuint32(s, static_cast<std::uint32_t>(v.size()));
            // Bulk write covers arithmetic primitives AND DWNC packed
            // records whose memory layout matches the wire layout.
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && ::psio::is_dwnc_v<E> && fully_fixed<E>() &&
               std::is_trivially_copyable_v<E> &&
               fixed_contrib<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               if (!v.empty())
               {
                  if constexpr (sink_counts_only_v<Sink>)
                     s.write(nullptr, v.size() * sizeof(E));
                  else
                     s.write(v.data(), v.size() * sizeof(E));
               }
            }
            else
            {
               for (const auto& x : v)
                  write_value(x, s, sizes, idx_ptr);
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            write_varuint32(s, static_cast<std::uint32_t>(v.size()));
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (is_std_optional<T>::value)
         {
            std::uint8_t tag = v.has_value() ? 1 : 0;
            s.write(reinterpret_cast<const char*>(&tag), 1);
            if (v.has_value())
               write_value(*v, s, sizes, idx_ptr);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            write_varuint32(s, static_cast<std::uint32_t>(v.index()));
            std::visit(
               [&](const auto& alt) {
                  write_value(alt, s, sizes, idx_ptr);
               },
               v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(v.data(), nbytes);
         }
         else if constexpr (is_bitlist<T>::value)
         {
            write_varuint32(s, static_cast<std::uint32_t>(v.size()));
            auto bytes = v.bytes();
            if (!bytes.empty())
               s.write(bytes.data(), bytes.size());
         }
         else if constexpr (Record<T>)
         {
            // ── Memcpy fast path for DWNC packed records ─────────────
            //
            // When the record is DWNC + all fields fully-fixed +
            // sizeof(T) == sum-of-field-sizes (i.e. user used
            // __attribute__((packed)) or got lucky with field order),
            // the in-memory bytes ARE the wire bytes. Skip the per-
            // field walker and emit one struct-wide memcpy. Mirrors
            // v1 bin's pack_bin_write_all batching for bitwise runs;
            // big win at vector-of-record scale (ValidatorList × 100).
            if constexpr (::psio::is_dwnc_v<T> && fully_fixed<T>() &&
                          std::is_trivially_copyable_v<T> &&
                          fixed_contrib<T>() == sizeof(T))
            {
               if constexpr (sink_counts_only_v<Sink>)
                  s.write(nullptr, sizeof(T));
               else
                  s.write(reinterpret_cast<const char*>(&v), sizeof(T));
               return;
            }
            using R = ::psio::reflect<T>;
            auto write_body = [&](auto& sink) {
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
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
                         if constexpr (::psio::has_as_override_v<eff>)
                         {
                            using Tag = ::psio::adapter_tag_of_t<eff>;
                            using Proj = ::psio::adapter<
                               std::remove_cvref_t<F>, Tag>;
                            if constexpr (sink_counts_only_v<
                                            std::decay_t<decltype(sink)>>)
                            {
                               const auto n = Proj::packsize(fref);
                               sink.write(
                                  nullptr,
                                  varuint32_size(
                                     static_cast<std::uint32_t>(n)) +
                                     n);
                            }
                            else
                            {
                               std::vector<char> tmp;
                               Proj::encode(fref, tmp);
                               write_varuint32(
                                  sink,
                                  static_cast<std::uint32_t>(tmp.size()));
                               sink.write(tmp.data(), tmp.size());
                            }
                         }
                         else
                         {
                            write_value(fref, sink, sizes, idx_ptr);
                         }
                      }()),
                     ...);
               }(std::make_index_sequence<R::member_count>{});
            };
            if constexpr (::psio::is_dwnc_v<T>)
            {
               // DWNC: no extensibility prefix, fields concatenate.
               write_body(s);
            }
            else
            {
               // Non-DWNC: wrap body in varuint content_size prefix.
               // body_size comes from the cache when available (the
               // size-collect pass populated it during the upfront
               // packed_size_of call); otherwise fall back to the
               // per-record record_body_size walk (the legacy path,
               // kept for size_stream and other no-cache callers).
               std::size_t body_size;
               if (sizes && idx_ptr)
                  body_size = sizes[(*idx_ptr)++];
               else
                  body_size = record_body_size(v);
               write_varuint32(
                  s, static_cast<std::uint32_t>(body_size));
               if constexpr (sink_counts_only_v<Sink>)
                  s.write(nullptr, body_size);
               else
                  write_body(s);
            }
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::bin: unsupported type in write_value");
         }
      }

      // Cursor-based decoder — every decode_value advances `pos`.
      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      // In-place decode helper. Same trick as borsh / bincode: skip
      // the temp + move-assign on std::string and bulk-memcpy
      // std::vector fields by writing straight into the destination.
      template <typename T>
      void decode_into(std::span<const char> src, std::size_t& pos,
                       T& out)
      {
         if constexpr (std::is_same_v<T, std::string>)
         {
            const std::uint32_t n = read_varuint32(src, pos);
            out.assign(src.data() + pos, src.data() + pos + n);
            pos += n;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E               = typename T::value_type;
            const std::uint32_t n = read_varuint32(src, pos);
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && ::psio::is_dwnc_v<E> && fully_fixed<E>() &&
               std::is_trivially_copyable_v<E> &&
               fixed_contrib<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               const E* first =
                  reinterpret_cast<const E*>(src.data() + pos);
               out.assign(first, first + n);
               pos += sizeof(E) * n;
            }
            else
            {
               out.clear();
               out.reserve(n);
               for (std::uint32_t i = 0; i < n; ++i)
                  out.push_back(decode_value<E>(src, pos));
            }
         }
         else
         {
            out = decode_value<T>(src, pos);
         }
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos)
      {
         if constexpr (::psio::format_should_dispatch_adapter_v<
                          ::psio::bin, T>)
         {
            using Proj = ::psio::adapter<std::remove_cvref_t<T>,
                                             ::psio::binary_category>;
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            auto out = Proj::decode(
               std::span<const char>(src.data() + pos, n));
            pos += n;
            return out;
         }

         if constexpr (std::is_same_v<T, bool>)
            return static_cast<unsigned char>(src[pos++]) != 0;
         else if constexpr (std::is_same_v<T, ::psio::uint256>)
         {
            T out{};
            std::memcpy(out.limb, src.data() + pos, 32);
            pos += 32;
            return out;
         }
         else if constexpr (std::is_same_v<T, ::psio::uint128> ||
                            std::is_same_v<T, ::psio::int128>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, 16);
            pos += 16;
            return out;
         }
         else if constexpr (std::is_arithmetic_v<T>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, sizeof(T));
            pos += sizeof(T);
            return out;
         }
         else if constexpr (is_std_array<T>::value)
         {
            T out{};
            for (std::size_t i = 0; i < std::tuple_size<T>::value; ++i)
               out[i] = decode_value<typename T::value_type>(src, pos);
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E             = typename T::value_type;
            const std::uint32_t n = read_varuint32(src, pos);
            // Bulk-memcpy fast path covers two cases:
            //   (a) arithmetic elements (raw LE primitives)
            //   (b) DWNC packed records whose memory layout matches the
            //       wire layout exactly (sizeof(E) == sum-of-fields and
            //       trivially-copyable). vector<Validator>×100 hits this.
            // assign(p, p+n) lowers to a single memcpy of n*sizeof(E)
            // bytes for trivially-copyable E, avoiding both resize's
            // zero-init pass and the per-element decode_value call.
            constexpr bool is_arith =
               std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
            constexpr bool is_memcpy_record =
               Record<E> && ::psio::is_dwnc_v<E> && fully_fixed<E>() &&
               std::is_trivially_copyable_v<E> &&
               fixed_contrib<E>() == sizeof(E);
            if constexpr (is_arith || is_memcpy_record)
            {
               const E* first = reinterpret_cast<const E*>(src.data() + pos);
               std::vector<E> out(first, first + n);
               pos += n * sizeof(E);
               return out;
            }
            else
            {
               std::vector<E> out;
               out.reserve(n);
               for (std::uint32_t i = 0; i < n; ++i)
                  out.push_back(decode_value<E>(src, pos));
               return out;
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            const std::uint32_t n = read_varuint32(src, pos);
            std::string out(src.data() + pos, src.data() + pos + n);
            pos += n;
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V            = typename T::value_type;
            const bool present = static_cast<unsigned char>(src[pos++]) != 0;
            if (!present)
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(src, pos)};
         }
         else if constexpr (is_std_variant<T>::value)
         {
            const auto idx =
               static_cast<std::size_t>(read_varuint32(src, pos));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  std::variant_alternative_t<Is, T>>(src,
                                                                       pos)},
                       true)
                    : false) ||
                  ...);
               (void)found;
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return out;
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            T                     out{};
            if constexpr (nbytes > 0)
            {
               std::memcpy(out.data(), src.data() + pos, nbytes);
               pos += nbytes;
            }
            return out;
         }
         else if constexpr (is_bitlist<T>::value)
         {
            const std::uint32_t bit_count = read_varuint32(src, pos);
            T                 out;
            const std::size_t byte_count = (bit_count + 7) / 8;
            auto&             bits       = out.storage();
            bits.resize(bit_count);
            for (std::uint32_t i = 0; i < bit_count; ++i)
            {
               const unsigned char b =
                  static_cast<unsigned char>(src[pos + (i >> 3)]);
               bits[i] = (b >> (i & 7)) & 1;
            }
            pos += byte_count;
            return out;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio::reflect<T>;
            // ── Memcpy fast path for DWNC packed records ─────────────
            // Mirror of the encode side. When wire layout matches
            // memory layout exactly, one memcpy beats nine per-field
            // reads.
            if constexpr (::psio::is_dwnc_v<T> && fully_fixed<T>() &&
                          std::is_trivially_copyable_v<T> &&
                          fixed_contrib<T>() == sizeof(T))
            {
               T out;
               std::memcpy(&out, src.data() + pos, sizeof(T));
               pos += sizeof(T);
               return out;
            }
            T       out{};
            // Non-DWNC records carry a varuint content_size prefix. Read
            // and skip it — we trust it as the extent for extensibility
            // tolerance (decoders that know fewer fields than the writer
            // can stop reading, decoders that know more can synthesize
            // defaults past content_end). Trailing-pruning /
            // synthesis is a follow-up; for now we just parse the
            // prefix and walk the known fields.
            if constexpr (!::psio::is_dwnc_v<T>)
            {
               (void)read_varuint32(src, pos);
            }
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      auto& fref =
                         out.*(R::template member_pointer<Is>);
                      using eff =
                         typename ::psio::effective_annotations_for<
                            T, F,
                            R::template member_pointer<Is>>::value_t;
                      if constexpr (::psio::has_as_override_v<eff>)
                      {
                         using Tag = ::psio::adapter_tag_of_t<eff>;
                         using Proj = ::psio::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         const std::uint32_t n =
                            read_varuint32(src, pos);
                         fref = Proj::decode(
                            std::span<const char>(src.data() + pos, n));
                         pos += n;
                      }
                      else
                      {
                         decode_into<F>(src, pos, fref);
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio::bin: unsupported type in decode_value");
         }
      }

   }  // namespace detail::bin_impl

   struct bin : format_tag_base<bin>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), bin, const T& v,
                             std::vector<char>& sink)
      {
         //  Stack-cached size precomputation: one walk populates the
         //  per-record body-size cache and computes the total; the
         //  write walk consumes the cache instead of recomputing
         //  record_body_size() at every nested non-DWNC record.
         constexpr std::size_t      kSlotSbo = 64;
         std::uint32_t              sbo[kSlotSbo];
         std::vector<std::uint32_t> heap;
         std::uint32_t*             sizes = sbo;
         const std::size_t          K =
            detail::bin_impl::bin_count_records(v);
         if (K > kSlotSbo)
         {
            heap.resize(K);
            sizes = heap.data();
         }
         std::size_t       idx = 0;
         const std::size_t total =
            detail::bin_impl::packed_size_of_collect(v, sizes, idx);
         const std::size_t orig = sink.size();
         sink.resize(orig + total);
         ::psio::fast_buf_stream fbs{sink.data() + orig, total};
         idx = 0;
         detail::bin_impl::write_value(v, fbs, sizes, &idx);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), bin,
                                          const T& v)
      {
         constexpr std::size_t      kSlotSbo = 64;
         std::uint32_t              sbo[kSlotSbo];
         std::vector<std::uint32_t> heap;
         std::uint32_t*             sizes = sbo;
         const std::size_t          K =
            detail::bin_impl::bin_count_records(v);
         if (K > kSlotSbo)
         {
            heap.resize(K);
            sizes = heap.data();
         }
         std::size_t       idx = 0;
         const std::size_t total =
            detail::bin_impl::packed_size_of_collect(v, sizes, idx);
         std::vector<char>       out(total);
         ::psio::fast_buf_stream fbs{out.data(), out.size()};
         idx = 0;
         detail::bin_impl::write_value(v, fbs, sizes, &idx);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), bin, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::bin_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), bin,
                                    const T& v)
      {
         return detail::bin_impl::packed_size_of(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), bin, T*,
                                     std::span<const char> bytes) noexcept
      {
         // Minimal structural check: non-empty for non-void types.
         if (bytes.empty())
            return codec_fail("bin: empty buffer", 0, "bin");
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "bin");
             !st.ok())
            return st;
         return codec_ok();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     bin, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("bin: empty buffer", 0, "bin");
         if constexpr (::psio::Reflected<T>)
         {
            try
            {
               std::size_t pos = 0;
               T decoded = detail::bin_impl::decode_value<T>(bytes, pos);
               return ::psio::validate_specs_on_value(decoded);
            }
            catch (...)
            {
               return codec_fail(
                  "bin: decode failed during validate_strict", 0, "bin");
            }
         }
         return codec_ok();
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio::make_boxed<T>),
                                           bin, T*,
                                           std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::bin_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio

// ── Forward declarations — dynamic CPO friend overloads live in
// dynamic_bin.hpp (it has to see dynamic_value + schema). These forward
// declarations are here only so ADL finds the name from ssz-tag-only
// call sites; the definitions are the separate header.

