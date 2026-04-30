#pragma once
//
// psio/wit.hpp — WIT canonical ABI format tag.
//
// WebAssembly Component Model canonical ABI (GC-unaware subset):
// fixed-layout records with fields at compile-time offsets, strings
// and lists as (u32 ptr, u32 len) pointing into the same buffer.
// Deterministic — same value always produces the same bytes.
//
// Supported shapes:
//   - primitives (arithmetic + bool), enums
//   - std::string (UTF-8, no trailing NUL)
//   - std::vector / std::array of any supported leaf
//   - std::optional, std::variant, std::tuple
//   - reflected records (root or nested, recursive)
//
// Wire reference:
// https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md
//
// Not supported: std::expected, resources (own<T>/borrow<T>) — the
// resource model requires a runtime ownership story that doesn't fit
// a pure serialization library. Attempting to encode an unsupported
// shape is a compile error — the Root concept guards the tag_invoke
// entry points.

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>
#include <psio/validate_strict_walker.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

   struct wit;

   namespace detail::wit_impl {

      // ── Shape classifiers ─────────────────────────────────────────────────

      template <typename T>
      struct is_vector : std::false_type {};
      template <typename E, typename A>
      struct is_vector<std::vector<E, A>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_array : std::false_type {};
      template <typename E, std::size_t N>
      struct is_array<std::array<E, N>> : std::true_type
      {
         using element_type             = E;
         static constexpr std::size_t n = N;
      };

      template <typename T>
      struct is_optional : std::false_type {};
      template <typename E>
      struct is_optional<std::optional<E>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_variant : std::false_type {};
      template <typename... Ts>
      struct is_variant<std::variant<Ts...>> : std::true_type {};

      template <typename T>
      struct is_tuple : std::false_type {};
      template <typename... Ts>
      struct is_tuple<std::tuple<Ts...>> : std::true_type {};

      template <typename T>
      concept Record = ::psio::Reflected<T> && !std::is_enum_v<T>;

      // What can appear at the root of a wit-encoded buffer. Records
      // are the common case; the canonical ABI also permits standalone
      // option / variant / tuple at function boundaries.
      template <typename T>
      concept Root = Record<T> || is_optional<T>::value ||
                     is_variant<T>::value || is_tuple<T>::value;

      // Discriminant width for a variant of N cases — Component Model
      // canonical ABI: u8 for ≤256 cases, u16 for ≤65536, else u32.
      consteval uint32_t discriminant_size(std::size_t n_cases)
      {
         if (n_cases <= (1u << 8))  return 1;
         if (n_cases <= (1u << 16)) return 2;
         return 4;
      }
      consteval uint32_t discriminant_align(std::size_t n_cases)
      {
         return discriminant_size(n_cases);
      }

      // Leaf = anything that fits in fixed-size cell (including string/vec
      // which have (ptr, len) as their fixed portion).
      template <typename T>
      concept Leaf = std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                     std::is_same_v<T, std::string> ||
                     is_vector<T>::value || Record<T>;

      // ── Canonical alignment and size (compile-time) ───────────────────────

      template <typename T>
      consteval uint32_t canonical_align();

      template <typename T>
      consteval uint32_t canonical_size();

      template <typename T>
      consteval uint32_t canonical_align()
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool> ||
                       std::is_same_v<U, std::int8_t> ||
                       std::is_same_v<U, std::uint8_t> ||
                       std::is_same_v<U, char>)
            return 1;
         else if constexpr (std::is_same_v<U, std::int16_t> ||
                            std::is_same_v<U, std::uint16_t>)
            return 2;
         else if constexpr (std::is_same_v<U, std::int32_t> ||
                            std::is_same_v<U, std::uint32_t> ||
                            std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, std::int64_t> ||
                            std::is_same_v<U, std::uint64_t> ||
                            std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_enum_v<U>)
            return canonical_align<std::underlying_type_t<U>>();
         else if constexpr (std::is_same_v<U, std::string>)
            return 4;  // (u32 ptr, u32 len)
         else if constexpr (is_vector<U>::value)
            return 4;  // (u32 ptr, u32 len)
         else if constexpr (is_array<U>::value)
            return canonical_align<typename is_array<U>::element_type>();
         else if constexpr (Record<U>)
         {
            using R                 = ::psio::reflect<U>;
            constexpr std::size_t N = R::member_count;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(
                    m,
                    canonical_align<typename R::template member_type<Is>>())),
                ...);
               return m;
            }(std::make_index_sequence<N>{});
         }
         else if constexpr (is_optional<U>::value)
         {
            using E = typename is_optional<U>::element_type;
            return std::max<uint32_t>(1, canonical_align<E>());
         }
         else if constexpr (is_variant<U>::value)
         {
            constexpr std::size_t N = std::variant_size_v<U>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = discriminant_align(N);
               ((m = std::max(
                    m,
                    canonical_align<std::variant_alternative_t<Is, U>>())),
                ...);
               return m;
            }(std::make_index_sequence<N>{});
         }
         else if constexpr (is_tuple<U>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<U>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(
                    m,
                    canonical_align<std::tuple_element_t<Is, U>>())),
                ...);
               return m;
            }(std::make_index_sequence<N>{});
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported type");
         }
      }

      template <typename T>
      consteval uint32_t canonical_size()
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool> ||
                       std::is_same_v<U, std::int8_t> ||
                       std::is_same_v<U, std::uint8_t> ||
                       std::is_same_v<U, char>)
            return 1;
         else if constexpr (std::is_same_v<U, std::int16_t> ||
                            std::is_same_v<U, std::uint16_t>)
            return 2;
         else if constexpr (std::is_same_v<U, std::int32_t> ||
                            std::is_same_v<U, std::uint32_t> ||
                            std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, std::int64_t> ||
                            std::is_same_v<U, std::uint64_t> ||
                            std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_enum_v<U>)
            return canonical_size<std::underlying_type_t<U>>();
         else if constexpr (std::is_same_v<U, std::string>)
            return 8;  // (u32 ptr, u32 len)
         else if constexpr (is_vector<U>::value)
            return 8;  // (u32 ptr, u32 len)
         else if constexpr (is_array<U>::value)
         {
            using E = typename is_array<U>::element_type;
            return static_cast<uint32_t>(is_array<U>::n) * canonical_size<E>();
         }
         else if constexpr (Record<U>)
         {
            using R                 = ::psio::reflect<U>;
            constexpr std::size_t N = R::member_count;
            uint32_t size = 0;
            uint32_t max_align = 1;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto process = [&]<std::size_t I>() {
                  using FT                 = typename R::template member_type<I>;
                  constexpr uint32_t fa    = canonical_align<FT>();
                  constexpr uint32_t fs    = canonical_size<FT>();
                  max_align                = std::max(max_align, fa);
                  size                     = ((size + fa - 1) & ~(fa - 1)) + fs;
               };
               (process.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            return (size + max_align - 1) & ~(max_align - 1);
         }
         else if constexpr (is_optional<U>::value)
         {
            // Component Model: option<T> = variant<unit, T>. Disc=u8,
            // payload at align(T). Total padded to align(option).
            using E                = typename is_optional<U>::element_type;
            constexpr uint32_t a   = canonical_align<U>();
            constexpr uint32_t ea  = canonical_align<E>();
            constexpr uint32_t es  = canonical_size<E>();
            constexpr uint32_t off = (1u + ea - 1u) & ~(ea - 1u);
            constexpr uint32_t sz  = off + es;
            return (sz + a - 1u) & ~(a - 1u);
         }
         else if constexpr (is_variant<U>::value)
         {
            // Component Model: variant<Ts...>. disc + padding to
            // max_alt_align + max(size(Ti)). Padded to align(variant).
            constexpr std::size_t N = std::variant_size_v<U>;
            constexpr uint32_t  ds  = discriminant_size(N);
            constexpr uint32_t  a   = canonical_align<U>();
            uint32_t max_alt_size  = 0;
            uint32_t max_alt_align = 1;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto process = [&]<std::size_t I>() {
                  using A = std::variant_alternative_t<I, U>;
                  max_alt_size  = std::max(max_alt_size,  canonical_size<A>());
                  max_alt_align = std::max(max_alt_align, canonical_align<A>());
               };
               (process.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            uint32_t off = (ds + max_alt_align - 1u) & ~(max_alt_align - 1u);
            uint32_t sz  = off + max_alt_size;
            return (sz + a - 1u) & ~(a - 1u);
         }
         else if constexpr (is_tuple<U>::value)
         {
            // Component Model: tuple<Ts...> = same as record's canonical
            // ABI — fields sequentially with alignment.
            constexpr std::size_t N = std::tuple_size_v<U>;
            uint32_t size = 0;
            uint32_t max_align = 1;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto process = [&]<std::size_t I>() {
                  using FT              = std::tuple_element_t<I, U>;
                  constexpr uint32_t fa = canonical_align<FT>();
                  constexpr uint32_t fs = canonical_size<FT>();
                  max_align             = std::max(max_align, fa);
                  size                  = ((size + fa - 1) & ~(fa - 1)) + fs;
               };
               (process.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            return (size + max_align - 1) & ~(max_align - 1);
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported type");
         }
      }

      // Offset of the payload byte within an option<T> (after disc + pad).
      template <typename T>
      consteval uint32_t optional_payload_offset()
      {
         using E               = typename is_optional<T>::element_type;
         constexpr uint32_t ea = canonical_align<E>();
         return (1u + ea - 1u) & ~(ea - 1u);
      }

      // Offset of the payload byte within a variant<Ts...>.
      template <typename T>
      consteval uint32_t variant_payload_offset()
      {
         constexpr std::size_t N  = std::variant_size_v<T>;
         constexpr uint32_t    ds = discriminant_size(N);
         uint32_t max_alt_align = 1;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((max_alt_align = std::max(
                 max_alt_align,
                 canonical_align<std::variant_alternative_t<Is, T>>())),
             ...);
         }(std::make_index_sequence<N>{});
         return (ds + max_alt_align - 1u) & ~(max_alt_align - 1u);
      }

      // Offset of tuple element I.
      template <typename T, std::size_t I>
      consteval uint32_t tuple_field_offset()
      {
         uint32_t offset = 0;
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            auto add = [&]<std::size_t J>() {
               using FT              = std::tuple_element_t<J, T>;
               constexpr uint32_t fa = canonical_align<FT>();
               constexpr uint32_t fs = canonical_size<FT>();
               offset                = (offset + fa - 1) & ~(fa - 1);
               offset += fs;
            };
            (add.template operator()<Js>(), ...);
         }(std::make_index_sequence<I>{});
         using FT_I              = std::tuple_element_t<I, T>;
         constexpr uint32_t fa_i = canonical_align<FT_I>();
         offset                  = (offset + fa_i - 1) & ~(fa_i - 1);
         return offset;
      }

      template <typename T, std::size_t I>
      consteval uint32_t canonical_field_offset()
      {
         using R         = ::psio::reflect<T>;
         uint32_t offset = 0;
         // Walk fields 0..I-1, accumulating offset with alignment padding.
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            auto add = [&]<std::size_t J>() {
               using FT              = typename R::template member_type<J>;
               constexpr uint32_t fa = canonical_align<FT>();
               constexpr uint32_t fs = canonical_size<FT>();
               offset                = (offset + fa - 1) & ~(fa - 1);
               offset += fs;
            };
            (add.template operator()<Js>(), ...);
         }(std::make_index_sequence<I>{});
         // Pad up to field I's own alignment.
         using FT_I              = typename R::template member_type<I>;
         constexpr uint32_t fa_i = canonical_align<FT_I>();
         offset                  = (offset + fa_i - 1) & ~(fa_i - 1);
         return offset;
      }

      // ── Packed size (runtime — includes variable tails) ────────────────────

      template <typename T>
      uint32_t packed_size_of(const T& value);

      template <typename T>
      uint32_t variable_tail_bytes(const T& value)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>)
         {
            (void)value;
            return 0;
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            return static_cast<uint32_t>(value.size());
         }
         else if constexpr (is_vector<U>::value)
         {
            using E                = typename U::value_type;
            constexpr uint32_t es  = canonical_size<E>();
            constexpr uint32_t ea  = canonical_align<E>();
            // Align the list's base offset (conservative upper bound on
            // the padding the bump allocator will insert).
            uint32_t pad           = ea > 1 ? (ea - 1) : 0;
            uint32_t total         = pad + static_cast<uint32_t>(value.size()) * es;
            for (const auto& e : value)
               total += variable_tail_bytes(e);
            return total;
         }
         else if constexpr (is_array<U>::value)
         {
            // std::array is packed in-place, no variable tail.
            uint32_t total = 0;
            for (const auto& e : value)
               total += variable_tail_bytes(e);
            return total;
         }
         else if constexpr (Record<U>)
         {
            using R                 = ::psio::reflect<U>;
            constexpr std::size_t N = R::member_count;
            uint32_t total = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((total += variable_tail_bytes(
                    value.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});
            return total;
         }
         else if constexpr (is_optional<U>::value)
         {
            return value.has_value() ? variable_tail_bytes(*value) : 0;
         }
         else if constexpr (is_variant<U>::value)
         {
            return std::visit(
               [](const auto& v) { return variable_tail_bytes(v); }, value);
         }
         else if constexpr (is_tuple<U>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<U>;
            uint32_t total = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((total += variable_tail_bytes(std::get<Is>(value))), ...);
            }(std::make_index_sequence<N>{});
            return total;
         }
         else
         {
            return 0;
         }
      }

      template <typename T>
      uint32_t packed_size_of(const T& value)
      {
         return canonical_size<T>() + variable_tail_bytes(value);
      }

      // ── Buffer helpers ────────────────────────────────────────────────────

      class bump_buf
      {
         std::vector<char> buf_;

        public:
         explicit bump_buf(uint32_t reserve = 0)
         {
            if (reserve)
               buf_.reserve(reserve);
         }

         uint32_t size() const { return static_cast<uint32_t>(buf_.size()); }

         // Allocate `n` bytes at current end, aligned up to `a`. Returns
         // the offset to the start of the allocation.
         uint32_t alloc(uint32_t a, uint32_t n)
         {
            uint32_t off = (size() + a - 1) & ~(a - 1);
            buf_.resize(off + n, 0);
            return off;
         }

         char*       data(uint32_t at) { return buf_.data() + at; }
         const char* data(uint32_t at) const { return buf_.data() + at; }

         std::vector<char> finish() { return std::move(buf_); }
      };

      template <typename T>
      void lower_leaf(bump_buf& buf, uint32_t at, const T& value);

      template <typename T>
      void lower_record(bump_buf& buf, uint32_t base, const T& value);

      template <typename E>
      void lower_list(bump_buf& buf, uint32_t at, const std::vector<E>& vec)
      {
         uint32_t count = static_cast<uint32_t>(vec.size());
         uint32_t ptr   = 0;
         if (count > 0)
         {
            constexpr uint32_t ea = canonical_align<E>();
            constexpr uint32_t es = canonical_size<E>();
            ptr                   = buf.alloc(ea, count * es);
            for (uint32_t i = 0; i < count; ++i)
               lower_leaf<E>(buf, ptr + i * es, vec[i]);
         }
         // Write header (ptr, count) at `at`.
         std::memcpy(buf.data(at), &ptr, 4);
         std::memcpy(buf.data(at + 4), &count, 4);
      }

      inline void lower_string(bump_buf& buf, uint32_t at,
                               std::string_view sv)
      {
         uint32_t len = static_cast<uint32_t>(sv.size());
         uint32_t ptr = 0;
         if (len > 0)
         {
            ptr = buf.alloc(1, len);
            std::memcpy(buf.data(ptr), sv.data(), len);
         }
         std::memcpy(buf.data(at), &ptr, 4);
         std::memcpy(buf.data(at + 4), &len, 4);
      }

      template <typename T>
      void lower_leaf(bump_buf& buf, uint32_t at, const T& value)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            uint8_t b = value ? 1 : 0;
            std::memcpy(buf.data(at), &b, 1);
         }
         else if constexpr (std::is_enum_v<U>)
         {
            using W = std::underlying_type_t<U>;
            W wire  = static_cast<W>(value);
            std::memcpy(buf.data(at), &wire, sizeof(W));
         }
         else if constexpr (std::is_arithmetic_v<U>)
         {
            std::memcpy(buf.data(at), &value, sizeof(U));
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            lower_string(buf, at, value);
         }
         else if constexpr (is_vector<U>::value)
         {
            lower_list(buf, at, value);
         }
         else if constexpr (is_array<U>::value)
         {
            using E               = typename is_array<U>::element_type;
            constexpr uint32_t es = canonical_size<E>();
            for (std::size_t i = 0; i < is_array<U>::n; ++i)
               lower_leaf<E>(buf, at + i * es, value[i]);
         }
         else if constexpr (Record<U>)
         {
            lower_record<U>(buf, at, value);
         }
         else if constexpr (is_optional<U>::value)
         {
            using E              = typename is_optional<U>::element_type;
            uint8_t disc         = value.has_value() ? 1 : 0;
            std::memcpy(buf.data(at), &disc, 1);
            constexpr uint32_t off = optional_payload_offset<U>();
            // Payload slot is always present (size is fixed). Zero-fill
            // when None to keep wire deterministic — encoders should
            // never emit uninitialized payload bytes.
            if (value.has_value())
               lower_leaf<E>(buf, at + off, *value);
            else
            {
               constexpr uint32_t es = canonical_size<E>();
               std::memset(buf.data(at + off), 0, es);
            }
         }
         else if constexpr (is_variant<U>::value)
         {
            constexpr std::size_t N  = std::variant_size_v<U>;
            constexpr uint32_t    ds = discriminant_size(N);
            // Write discriminant (1/2/4 bytes, little-endian).
            uint32_t idx = static_cast<uint32_t>(value.index());
            std::memcpy(buf.data(at), &idx, ds);
            // Zero-fill the payload slot first so unused alt bytes are
            // deterministic, then write the live alternative.
            constexpr uint32_t off  = variant_payload_offset<U>();
            constexpr uint32_t total = canonical_size<U>();
            std::memset(buf.data(at + off), 0, total - off);
            std::visit(
               [&](const auto& alt) {
                  using A = std::remove_cvref_t<decltype(alt)>;
                  lower_leaf<A>(buf, at + off, alt);
               },
               value);
         }
         else if constexpr (is_tuple<U>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<U>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (lower_leaf<std::tuple_element_t<Is, U>>(
                    buf, at + tuple_field_offset<U, Is>(),
                    std::get<Is>(value)),
                ...);
            }(std::make_index_sequence<N>{});
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported leaf");
         }
      }

      template <typename T>
      void lower_record(bump_buf& buf, uint32_t base, const T& value)
      {
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (lower_leaf(
                 buf, base + canonical_field_offset<T, Is>(),
                 value.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Lifters (decode) ──────────────────────────────────────────────────

      template <typename T>
      void lift_leaf(std::span<const char> buf, uint32_t at, T& out);

      template <typename T>
      void lift_record(std::span<const char> buf, uint32_t base, T& out);

      inline uint32_t read_u32(std::span<const char> buf, uint32_t at)
      {
         uint32_t v;
         std::memcpy(&v, buf.data() + at, 4);
         return v;
      }

      template <typename E>
      void lift_list(std::span<const char> buf, uint32_t at,
                     std::vector<E>& out)
      {
         uint32_t ptr   = read_u32(buf, at);
         uint32_t count = read_u32(buf, at + 4);
         out.resize(count);
         if (count == 0)
            return;
         constexpr uint32_t es = canonical_size<E>();
         for (uint32_t i = 0; i < count; ++i)
            lift_leaf<E>(buf, ptr + i * es, out[i]);
      }

      inline void lift_string(std::span<const char> buf, uint32_t at,
                              std::string& out)
      {
         uint32_t ptr = read_u32(buf, at);
         uint32_t len = read_u32(buf, at + 4);
         out.assign(buf.data() + ptr, len);
      }

      template <typename T>
      void lift_leaf(std::span<const char> buf, uint32_t at, T& out)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            uint8_t b;
            std::memcpy(&b, buf.data() + at, 1);
            out = (b != 0);
         }
         else if constexpr (std::is_enum_v<U>)
         {
            using W = std::underlying_type_t<U>;
            W wire;
            std::memcpy(&wire, buf.data() + at, sizeof(W));
            out = static_cast<U>(wire);
         }
         else if constexpr (std::is_arithmetic_v<U>)
         {
            std::memcpy(&out, buf.data() + at, sizeof(U));
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            lift_string(buf, at, out);
         }
         else if constexpr (is_vector<U>::value)
         {
            lift_list(buf, at, out);
         }
         else if constexpr (is_array<U>::value)
         {
            using E               = typename is_array<U>::element_type;
            constexpr uint32_t es = canonical_size<E>();
            for (std::size_t i = 0; i < is_array<U>::n; ++i)
               lift_leaf<E>(buf, at + i * es, out[i]);
         }
         else if constexpr (Record<U>)
         {
            lift_record<U>(buf, at, out);
         }
         else if constexpr (is_optional<U>::value)
         {
            using E      = typename is_optional<U>::element_type;
            uint8_t disc = static_cast<uint8_t>(buf[at]);
            if (disc == 0)
               out = std::nullopt;
            else
            {
               constexpr uint32_t off = optional_payload_offset<U>();
               E inner{};
               lift_leaf<E>(buf, at + off, inner);
               out = std::move(inner);
            }
         }
         else if constexpr (is_variant<U>::value)
         {
            constexpr std::size_t N  = std::variant_size_v<U>;
            constexpr uint32_t    ds = discriminant_size(N);
            uint32_t idx = 0;
            std::memcpy(&idx, buf.data() + at, ds);
            constexpr uint32_t off = variant_payload_offset<U>();
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((idx == Is
                    ? (out = U{std::in_place_index<Is>,
                                ([&] {
                                    using A =
                                       std::variant_alternative_t<Is, U>;
                                    A tmp{};
                                    lift_leaf<A>(buf, at + off, tmp);
                                    return tmp;
                                 }())},
                       true)
                    : false) ||
                ...);
            }(std::make_index_sequence<N>{});
         }
         else if constexpr (is_tuple<U>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<U>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (lift_leaf<std::tuple_element_t<Is, U>>(
                    buf, at + tuple_field_offset<U, Is>(),
                    std::get<Is>(out)),
                ...);
            }(std::make_index_sequence<N>{});
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported leaf");
         }
      }

      template <typename T>
      void lift_record(std::span<const char> buf, uint32_t base, T& out)
      {
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (lift_leaf(buf, base + canonical_field_offset<T, Is>(),
                       out.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Validate (bounds only for MVP) ────────────────────────────────────

      template <typename T>
      bool validate_leaf(std::span<const char> buf, uint32_t at);

      template <typename T>
      bool validate_record(std::span<const char> buf, uint32_t base)
      {
         using R                 = ::psio::reflect<T>;
         constexpr std::size_t N = R::member_count;
         bool                  ok = true;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((ok = ok && validate_leaf<typename R::template member_type<Is>>(
                            buf, base + canonical_field_offset<T, Is>())),
             ...);
         }(std::make_index_sequence<N>{});
         return ok;
      }

      template <typename T>
      bool validate_leaf(std::span<const char> buf, uint32_t at)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>)
         {
            return static_cast<std::size_t>(at) + canonical_size<U>() <=
                   buf.size();
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            if (static_cast<std::size_t>(at) + 8 > buf.size())
               return false;
            uint32_t ptr = read_u32(buf, at);
            uint32_t len = read_u32(buf, at + 4);
            if (len == 0)
               return true;
            return static_cast<std::size_t>(ptr) + len <= buf.size();
         }
         else if constexpr (is_vector<U>::value)
         {
            if (static_cast<std::size_t>(at) + 8 > buf.size())
               return false;
            uint32_t ptr   = read_u32(buf, at);
            uint32_t count = read_u32(buf, at + 4);
            if (count == 0)
               return true;
            using E = typename U::value_type;
            constexpr uint32_t es = canonical_size<E>();
            if (static_cast<std::size_t>(ptr) +
                   static_cast<std::size_t>(count) * es >
                buf.size())
               return false;
            for (uint32_t i = 0; i < count; ++i)
               if (!validate_leaf<E>(buf, ptr + i * es))
                  return false;
            return true;
         }
         else if constexpr (is_array<U>::value)
         {
            using E               = typename is_array<U>::element_type;
            constexpr uint32_t es = canonical_size<E>();
            if (static_cast<std::size_t>(at) +
                   static_cast<std::size_t>(is_array<U>::n) * es >
                buf.size())
               return false;
            for (std::size_t i = 0; i < is_array<U>::n; ++i)
               if (!validate_leaf<E>(buf, at + i * es))
                  return false;
            return true;
         }
         else if constexpr (Record<U>)
         {
            return validate_record<U>(buf, at);
         }
         else if constexpr (is_optional<U>::value)
         {
            constexpr uint32_t total = canonical_size<U>();
            if (static_cast<std::size_t>(at) + total > buf.size())
               return false;
            uint8_t disc = static_cast<uint8_t>(buf[at]);
            if (disc > 1)
               return false;  // option discriminant is 0 or 1 only
            if (disc == 1)
            {
               using E = typename is_optional<U>::element_type;
               constexpr uint32_t off = optional_payload_offset<U>();
               return validate_leaf<E>(buf, at + off);
            }
            return true;
         }
         else if constexpr (is_variant<U>::value)
         {
            constexpr std::size_t N  = std::variant_size_v<U>;
            constexpr uint32_t    ds = discriminant_size(N);
            constexpr uint32_t total = canonical_size<U>();
            if (static_cast<std::size_t>(at) + total > buf.size())
               return false;
            uint32_t idx = 0;
            std::memcpy(&idx, buf.data() + at, ds);
            if (idx >= N)
               return false;
            constexpr uint32_t off = variant_payload_offset<U>();
            bool ok = false;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((idx == Is
                    ? (ok = validate_leaf<std::variant_alternative_t<Is, U>>(
                              buf, at + off),
                       true)
                    : false) ||
                ...);
            }(std::make_index_sequence<N>{});
            return ok;
         }
         else if constexpr (is_tuple<U>::value)
         {
            constexpr std::size_t N = std::tuple_size_v<U>;
            bool ok = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((ok = ok &&
                      validate_leaf<std::tuple_element_t<Is, U>>(
                          buf, at + tuple_field_offset<U, Is>())),
                ...);
            }(std::make_index_sequence<N>{});
            return ok;
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported type");
         }
      }

   }  // namespace detail::wit_impl

   // ── Format tag ──────────────────────────────────────────────────────────

   struct wit : format_tag_base<wit>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
         requires detail::wit_impl::Root<T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode), wit,
                                          const T& v)
      {
         using namespace detail::wit_impl;
         bump_buf buf(packed_size_of(v));
         uint32_t root = buf.alloc(canonical_align<T>(), canonical_size<T>());
         if constexpr (detail::wit_impl::Record<T>)
            lower_record<T>(buf, root, v);
         else
            lower_leaf<T>(buf, root, v);
         return buf.finish();
      }

      template <typename T>
         requires detail::wit_impl::Root<T>
      friend T tag_invoke(decltype(::psio::decode<T>), wit, T*,
                          std::span<const char> bytes)
      {
         using namespace detail::wit_impl;
         T out{};
         if constexpr (detail::wit_impl::Record<T>)
            lift_record<T>(bytes, 0, out);
         else
            lift_leaf<T>(bytes, 0, out);
         return out;
      }

      template <typename T>
         requires detail::wit_impl::Root<T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), wit,
                                    const T& v)
      {
         return detail::wit_impl::packed_size_of(v);
      }

      template <typename T>
         requires detail::wit_impl::Root<T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>), wit,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         using namespace detail::wit_impl;
         if (auto st = ::psio::check_max_dynamic_cap<T>(bytes.size(), "wit");
             !st.ok())
            return st;
         bool ok;
         if constexpr (detail::wit_impl::Record<T>)
            ok = validate_record<T>(bytes, 0);
         else
            ok = validate_leaf<T>(bytes, 0);
         if (!ok)
            return codec_fail("wit: invalid buffer", 0, "wit");
         return codec_ok();
      }

      template <typename T>
         requires detail::wit_impl::Root<T>
      friend codec_status
      tag_invoke(decltype(::psio::validate_strict<T>), wit, T*,
                 std::span<const char> bytes) noexcept
      {
         using namespace detail::wit_impl;
         bool ok;
         if constexpr (detail::wit_impl::Record<T>)
            ok = validate_record<T>(bytes, 0);
         else
            ok = validate_leaf<T>(bytes, 0);
         if (!ok)
            return codec_fail("wit: invalid buffer", 0, "wit");
         try
         {
            T out{};
            if constexpr (detail::wit_impl::Record<T>)
               lift_record<T>(bytes, 0, out);
            else
               lift_leaf<T>(bytes, 0, out);
            return ::psio::validate_specs_on_value(out);
         }
         catch (...)
         {
            return codec_fail(
               "wit: decode failed during validate_strict", 0, "wit");
         }
      }

      template <typename T>
         requires detail::wit_impl::Root<T>
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio::make_boxed<T>), wit, T*,
                 std::span<const char> bytes) noexcept
      {
         using namespace detail::wit_impl;
         auto v = std::make_unique<T>();
         if constexpr (detail::wit_impl::Record<T>)
            lift_record<T>(bytes, 0, *v);
         else
            lift_leaf<T>(bytes, 0, *v);
         return v;
      }
   };

}  // namespace psio
