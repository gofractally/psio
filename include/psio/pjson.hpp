#pragma once
//
// psio/pjson.hpp — pjson value format.
//
// A pjson value is a (ptr, size) span where:
//
//   ptr[0]          tag byte  (high nibble = type, low nibble = payload-specific)
//   ptr[1 .. size)  raw value bits per the tag type
//
// Containers (array, object) embed an index at the *front* of their
// raw-value-bits region:
//
//   [num_fields:  2-bit-prefix varuint]      1..4 bytes
//   [hash[N]:     N × u8]                    objects only
//   [slot[N]:     N × u32 LE]                packed { offset:24, key_size:8 }
//                                            (key_size unused for arrays)
//   [value_data:  contiguous bytes]
//     for each i in 0..N:
//       object: [key bytes (key_size)][tag][raw value bits]
//               or, when key_size == 0xFF:
//                  [varuint excess][key bytes (0xFF + excess)][tag][raw]
//       array:  [tag][raw value bits]
//
// Entry size of child i: slot[i+1].offset - slot[i].offset
//                         (or, for the last child,
//                          container_value_data_size - slot[N-1].offset).
//
// Tag types:
//   0  null              raw = 0 bytes
//   1  bool              raw = 0;  low nibble: 0=false, 1=true
//   3  uint_inline       raw = 0;  unsigned value in low nibble (0..15)
//   4  int               raw = (low_nibble+1) bytes;  zigzag-LE mantissa
//   5  decimal           raw = (low_nibble+1) mantissa bytes + varscale (1..4)
//   6  ieee_float        raw = 8 bytes
//   8  string            raw = (size - 1) bytes; low nibble flag:
//                          0=raw_text, 1=escape_form
//   A  bytes             raw = (size - 1) bytes; low nibble reserved (must be 0)
//   B  array             raw = container content; low nibble selects layout:
//                          0      = generic array (per-element tags + slot table)
//                          1..10  = typed homogeneous array — element type
//                                   code = low_nibble - 1
//                                   (0=i8 1=i16 2=i32 3=i64
//                                    4=u8 5=u16 6=u32 7=u64
//                                    8=f32 9=f64)
//                          11..15 = reserved
//   C  object            raw = container content
//   2, 7, 9, D..F        reserved
//
// No magic, no version, no flags. Versioning lives in the application
// wrapper (HTTP content-type, file header, RPC envelope). Buffer length
// is the value's size — caller's responsibility to provide.
//
// This file owns:
//   * pjson_number             — unified numeric value type (i128 + i32)
//   * pjson_value              — variant tree (test/build helper, not the
//                                primary consumer surface; that's pjson_view)
//   * pjson::encode / decode / validate
//   * Internal encode / decode helpers used by pjson_view, pjson_typed,
//     pjson_json.

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#define XXH_INLINE_ALL
#include <hash/xxhash.h>

namespace psio {

   // ── numeric value type ────────────────────────────────────────────────

   struct pjson_number
   {
      __int128     mantissa = 0;
      std::int32_t scale    = 0;

      pjson_number() = default;
      constexpr pjson_number(__int128 m, std::int32_t s = 0) noexcept
          : mantissa(m), scale(s)
      {
      }
      template <typename T>
         requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
      constexpr pjson_number(T v) noexcept
          : mantissa(static_cast<__int128>(v)), scale(0)
      {
      }

      static pjson_number from_double(double d);
      static pjson_number from_string(std::string_view s);

      bool is_integer() const noexcept { return scale == 0; }

      std::optional<std::int64_t> to_int64() const noexcept
      {
         if (scale != 0) return std::nullopt;
         if (mantissa < std::numeric_limits<std::int64_t>::min() ||
             mantissa > std::numeric_limits<std::int64_t>::max())
            return std::nullopt;
         return static_cast<std::int64_t>(mantissa);
      }

      double to_double() const noexcept
      {
         double m = static_cast<double>(mantissa);
         if (scale == 0) return m;
         double       p    = 1.0;
         std::int32_t s    = scale > 0 ? scale : -scale;
         double       base = 10.0;
         while (s)
         {
            if (s & 1) p *= base;
            base *= base;
            s >>= 1;
         }
         return scale > 0 ? m * p : m / p;
      }

      friend bool operator==(const pjson_number&,
                             const pjson_number&) noexcept = default;

      friend std::strong_ordering compare(const pjson_number& a,
                                          const pjson_number& b) noexcept
      {
         if (a.scale == b.scale) return a.mantissa <=> b.mantissa;
         std::int32_t diff =
             a.scale < b.scale ? b.scale - a.scale : a.scale - b.scale;
         __int128 mult = 1;
         for (std::int32_t i = 0; i < diff && mult > 0; ++i) mult *= 10;
         if (a.scale < b.scale) return a.mantissa <=> (b.mantissa * mult);
         return (a.mantissa * mult) <=> b.mantissa;
      }
      friend bool numerically_equal(const pjson_number& a,
                                    const pjson_number& b) noexcept
      {
         return compare(a, b) == 0;
      }
   };

   // ── pjson_value variant tree (build/test helper) ──────────────────────

   struct pjson_value;
   struct pjson_null
   {
      friend bool operator==(pjson_null, pjson_null) noexcept { return true; }
   };
   using pjson_object = std::vector<std::pair<std::string, pjson_value>>;
   using pjson_array  = std::vector<pjson_value>;
   using pjson_bytes  = std::vector<std::uint8_t>;

   struct pjson_value
   {
      using variant_t = std::variant<pjson_null,
                                     bool,
                                     std::int64_t,
                                     double,
                                     pjson_number,
                                     std::string,
                                     pjson_bytes,
                                     pjson_array,
                                     pjson_object>;
      variant_t v;

      pjson_value() : v(pjson_null{}) {}
      template <typename T>
         requires(!std::is_same_v<std::decay_t<T>, pjson_value>)
      pjson_value(T&& x) : v(std::forward<T>(x))
      {
      }

      template <typename T>
      bool holds() const noexcept
      {
         return std::holds_alternative<T>(v);
      }
      template <typename T>
      const T& as() const
      {
         return std::get<T>(v);
      }
      template <typename T>
      T& as()
      {
         return std::get<T>(v);
      }
      bool is_null() const noexcept { return holds<pjson_null>(); }

      friend bool operator==(const pjson_value&, const pjson_value&) = default;
   };

   // ── implementation detail ─────────────────────────────────────────────

   namespace pjson_detail {

      // Allocator that *skips* zero-init on default-construction.
      // std::vector<T>::resize(n) calls allocator::construct(p) with no
      // args for each new element, which for the default allocator
      // value-initializes T (zero-init for trivial types). When we
      // resize-then-overwrite (the common pattern in this encoder), the
      // zero-init is wasted work. With this allocator, construct(p) is
      // a no-op; subsequent writes (memcpy / direct stores) are the
      // ONLY initialization those bytes ever see.
      //
      // Forwards every other construct() form to standard placement new
      // so vector::insert, push_back, emplace, etc. still work.
      template <typename T>
      struct uninit_alloc
      {
         using value_type = T;

         uninit_alloc() noexcept = default;
         template <typename U>
         uninit_alloc(const uninit_alloc<U>&) noexcept
         {
         }

         T* allocate(std::size_t n)
         {
            return std::allocator<T>{}.allocate(n);
         }
         void deallocate(T* p, std::size_t n) noexcept
         {
            std::allocator<T>{}.deallocate(p, n);
         }

         template <typename U>
         void construct(U*) noexcept
         { /* skip default-init */
         }
         template <typename U, typename A, typename... Args>
         void construct(U* p, A&& a, Args&&... args)
         {
            ::new ((void*)p) U(std::forward<A>(a),
                               std::forward<Args>(args)...);
         }
         template <typename U>
         void destroy(U* p) noexcept
         {
            if constexpr (!std::is_trivially_destructible_v<U>) p->~U();
         }

         template <typename U>
         bool operator==(const uninit_alloc<U>&) const noexcept
         {
            return true;
         }
         template <typename U>
         bool operator!=(const uninit_alloc<U>&) const noexcept
         {
            return false;
         }
      };

      // Tag-byte type codes (high nibble of the tag byte).
      // Low-nibble usage is per-type; see notes below.
      enum : std::uint8_t
      {
         t_null        = 0,
         t_bool        = 1,    // low nibble: 0 = false, 1 = true
         // 2 reserved
         t_uint_inline = 3,    // low nibble: unsigned value 0..15
         t_int         = 4,    // low nibble: byte_count - 1 (1..16)
         t_decimal     = 5,    // low nibble: mantissa byte_count - 1
         t_ieee_float  = 6,
         t_string      = 8,    // low nibble: text encoding flag
         // 9 reserved
         t_bytes       = 0xA,  // low nibble reserved (must be 0)
         t_array       = 0xB,  // low nibble: 0 = generic, 1..10 = typed
                               //   (element type code = low_nibble - 1)
         t_object      = 0xC,
      };

      // Element type codes for the typed-array form of t_array (when
      // the tag's low nibble is non-zero). On the wire the low nibble
      // carries `tac + 1` so that low-nibble = 0 stays reserved for
      // the generic array layout. Element bytes are little-endian
      // fixed-width values; the body is N × sizeof(element) raw bytes
      // followed by a u16 count.
      enum : std::uint8_t
      {
         tac_i8  = 0,
         tac_i16 = 1,
         tac_i32 = 2,
         tac_i64 = 3,
         tac_u8  = 4,
         tac_u16 = 5,
         tac_u32 = 6,
         tac_u64 = 7,
         tac_f32 = 8,
         tac_f64 = 9,
         // 10..14 reserved (low nibble values 11..15 on the wire)
         tac_invalid = 0xFF,
      };

      // Per-element byte count for a typed-array element type code.
      // Returns 0 for reserved / invalid codes; callers should check.
      inline std::size_t typed_array_elem_size(std::uint8_t code) noexcept
      {
         switch (code)
         {
            case tac_i8:  case tac_u8:  return 1;
            case tac_i16: case tac_u16: return 2;
            case tac_i32: case tac_u32: case tac_f32: return 4;
            case tac_i64: case tac_u64: case tac_f64: return 8;
            default: return 0;
         }
      }
      inline bool typed_array_is_signed_int(std::uint8_t code) noexcept
      {
         return code <= tac_i64;
      }
      inline bool typed_array_is_unsigned_int(std::uint8_t code) noexcept
      {
         return code >= tac_u8 && code <= tac_u64;
      }
      inline bool typed_array_is_float(std::uint8_t code) noexcept
      {
         return code == tac_f32 || code == tac_f64;
      }
      // Decode a t_array tag's low nibble into a tac_* code, returning
      // tac_invalid for the generic-array slot (low_nibble == 0) and
      // for reserved values (low_nibble > 10).
      inline std::uint8_t typed_array_code_from_low(
          std::uint8_t low) noexcept
      {
         if (low == 0 || low > 10) return tac_invalid;
         return static_cast<std::uint8_t>(low - 1);
      }
      // Inverse: tac_* → low-nibble (always low_nibble = tac + 1).
      inline std::uint8_t typed_array_low_from_code(
          std::uint8_t code) noexcept
      {
         return static_cast<std::uint8_t>(code + 1);
      }

      // Sub-encoding flags for t_string (low nibble of the tag).
      // Tells the JSON emitter what to do with the stored bytes;
      // tells consumers how to interpret them.
      enum : std::uint8_t
      {
         // Text bytes that have NOT been JSON-escaped — JSON emit
         // must run a per-character escape pass.
         string_flag_raw_text       = 0,
         // Text bytes already in JSON-escape form (`\n`, `\"`,
         // `\uXXXX` etc are LITERAL bytes in the buffer). JSON emit
         // wraps in quotes verbatim, no escape work.
         string_flag_escape_form    = 1,
         // 2..15: reserved
      };

      // Strip a trailing ".tag" suffix from a key for the prefilter hash.
      // "amount.decimal" → hash bucket of "amount". Stored keys with a
      // presentation suffix and bare-name lookups land in the same hash
      // bucket; byte-exact verify still distinguishes them.
      inline std::string_view strip_key_suffix(std::string_view k) noexcept
      {
         auto dot = k.rfind('.');
         return dot == std::string_view::npos ? k : k.substr(0, dot);
      }

      // 8-bit prefilter hash: low byte of XXH3-64 over the suffix-stripped
      // key.
      inline std::uint8_t key_hash8(std::string_view k) noexcept
      {
         auto base = strip_key_suffix(k);
         return static_cast<std::uint8_t>(
             XXH3_64bits(base.data(), base.size()));
      }

      // ── 2-bit-prefix varuint ──────────────────────────────────────────
      //
      // First byte: high 2 bits = total byte count - 1 (0..3 → 1..4),
      //             low 6 bits  = lowest 6 bits of value.
      // Subsequent bytes: next 8 bits each (LE).
      //
      // Capacities: 6 / 14 / 22 / 30 bits.
      // Used for: num_fields counts, decimal varscale, long-key excess.

      inline std::size_t varuint62_byte_count(std::uint64_t v) noexcept
      {
         if (v <= 0x3Fu)        return 1;
         if (v <= 0x3FFFu)      return 2;
         if (v <= 0x3FFFFFu)    return 3;
         return 4;
      }
      inline std::size_t write_varuint62(std::uint8_t* dst,
                                         std::size_t   pos,
                                         std::uint64_t v) noexcept
      {
         std::size_t  n      = varuint62_byte_count(v);
         std::uint8_t prefix = static_cast<std::uint8_t>((n - 1) << 6);
         dst[pos]            = prefix | static_cast<std::uint8_t>(v & 0x3Fu);
         std::uint64_t hi    = v >> 6;
         for (std::size_t i = 1; i < n; ++i)
         {
            dst[pos + i] = static_cast<std::uint8_t>(hi & 0xFFu);
            hi >>= 8;
         }
         return n;
      }
      // Read into *out*; returns bytes consumed, 0 on failure.
      inline std::size_t read_varuint62(const std::uint8_t* buf,
                                        std::size_t         len,
                                        std::uint64_t&      out) noexcept
      {
         if (len == 0) return 0;
         std::uint8_t b0 = buf[0];
         std::size_t  n  = static_cast<std::size_t>(b0 >> 6) + 1;
         if (n > len) return 0;
         std::uint64_t v = b0 & 0x3Fu;
         for (std::size_t i = 1; i < n; ++i)
            v |= static_cast<std::uint64_t>(buf[i]) << (6 + (i - 1) * 8);
         out = v;
         return n;
      }

      // ── 2-bit-prefix varint (signed) — used for decimal scale ─────────
      inline std::size_t varint62_byte_count(std::int32_t v) noexcept
      {
         std::uint64_t zz =
             (static_cast<std::uint64_t>(v) << 1) ^
             static_cast<std::uint64_t>(static_cast<std::int64_t>(v) >> 31);
         return varuint62_byte_count(zz);
      }
      inline std::size_t write_varint62(std::uint8_t* dst,
                                        std::size_t   pos,
                                        std::int32_t  v) noexcept
      {
         std::uint64_t zz =
             (static_cast<std::uint64_t>(v) << 1) ^
             static_cast<std::uint64_t>(static_cast<std::int64_t>(v) >> 31);
         return write_varuint62(dst, pos, zz);
      }
      inline std::size_t read_varint62(const std::uint8_t* buf,
                                       std::size_t         len,
                                       std::int32_t&       out) noexcept
      {
         std::uint64_t zz;
         std::size_t   n = read_varuint62(buf, len, zz);
         if (n == 0) return 0;
         std::int64_t s = static_cast<std::int64_t>(
             (zz >> 1) ^ (~(zz & 1) + 1));
         out = static_cast<std::int32_t>(s);
         return n;
      }

      // ── zigzag (i128) ─────────────────────────────────────────────────
      inline __uint128_t zz128_encode(__int128 v) noexcept
      {
         return (static_cast<__uint128_t>(v) << 1) ^
                static_cast<__uint128_t>(v >> 127);
      }
      inline __int128 zz128_decode(__uint128_t v) noexcept
      {
         return static_cast<__int128>((v >> 1) ^ (~(v & 1) + 1));
      }
      inline std::uint8_t mantissa_byte_count(__uint128_t zz) noexcept
      {
         if (zz == 0) return 1;
         std::uint64_t hi = static_cast<std::uint64_t>(zz >> 64);
         std::uint64_t lo = static_cast<std::uint64_t>(zz);
         int bits = hi != 0 ? (128 - std::countl_zero(hi))
                            : (64 - std::countl_zero(lo));
         return static_cast<std::uint8_t>((bits + 7) / 8);
      }

      // ── slot packing { offset:24, key_size:8 } ───────────────────────
      inline std::uint32_t pack_slot(std::uint32_t offset,
                                     std::uint8_t  key_size) noexcept
      {
         return (offset & 0x00FFFFFFu) |
                (static_cast<std::uint32_t>(key_size) << 24);
      }
      inline std::uint32_t slot_offset(std::uint32_t s) noexcept
      {
         return s & 0x00FFFFFFu;
      }
      inline std::uint8_t slot_key_size(std::uint32_t s) noexcept
      {
         return static_cast<std::uint8_t>(s >> 24);
      }

      // LE u32 read/write.
      inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept
      {
         std::uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }
      inline void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept
      {
         std::memcpy(p, &v, 4);
      }

      // Adaptive-width helpers used by all generic-container slot tables
      // and by row_array offsets. Width code: 0=u8, 1=u16, 2=u24, 3=u32.
      inline std::uint8_t width_code_for(std::uint64_t v) noexcept
      {
         if (v <= 0xFFu)        return 0;
         if (v <= 0xFFFFu)      return 1;
         if (v <= 0xFFFFFFu)    return 2;
         return 3;
      }
      inline std::size_t width_bytes(std::uint8_t code) noexcept
      {
         return static_cast<std::size_t>(code) + 1;
      }
      inline std::uint32_t read_width(const std::uint8_t* p,
                                      std::uint8_t        code) noexcept
      {
         std::uint32_t v = 0;
         std::memcpy(&v, p, width_bytes(code));
         return v;
      }
      inline void write_width(std::uint8_t* p,
                              std::uint8_t  code,
                              std::uint32_t v) noexcept
      {
         std::memcpy(p, &v, width_bytes(code));
      }

      // ── value sizing (recursion over pjson_value) ─────────────────────
      inline std::size_t value_size(const pjson_value& v) noexcept;

      inline std::size_t uint_inline_size() noexcept { return 1; }
      inline std::size_t int_size(std::int64_t i) noexcept
      {
         if (i >= 0 && i <= 15) return 1;
         std::uint64_t zz =
             (static_cast<std::uint64_t>(i) << 1) ^
             static_cast<std::uint64_t>(i >> 63);
         std::uint8_t bc = 1;
         std::uint64_t t = zz >> 8;
         while (t) { ++bc; t >>= 8; }
         return 1u + bc;
      }
      inline std::size_t number_size(const pjson_number& n) noexcept
      {
         if (n.scale == 0 && n.mantissa >= 0 && n.mantissa <= 15)
            return 1;
         __uint128_t  zz = zz128_encode(n.mantissa);
         std::uint8_t bc = mantissa_byte_count(zz);
         if (n.scale == 0) return 1u + bc;
         return 1u + bc + varint62_byte_count(n.scale);
      }
      inline std::size_t string_size(std::string_view s) noexcept
      {
         return 1u + s.size();
      }
      inline std::size_t bytes_size(const pjson_bytes& b) noexcept
      {
         return 1u + b.size();
      }

      // Container "content" size = num_fields_varuint + hash[N] + slot[N]*4
      //                          + sum(entry_sizes).
      // Container "value" size = 1 (tag) + content size.
      inline std::size_t container_value_data_size_array(
          const pjson_array& a) noexcept
      {
         std::size_t s = 0;
         for (const auto& el : a) s += value_size(el);
         return s;
      }
      inline std::size_t container_value_data_size_object(
          const pjson_object& o) noexcept
      {
         std::size_t s = 0;
         for (const auto& [k, v] : o)
         {
            std::size_t klen_extra = 0;
            if (k.size() >= 0xFFu)
               klen_extra = varuint62_byte_count(k.size() - 0xFFu);
            s += klen_extra + k.size() + value_size(v);
         }
         return s;
      }

      // Tail-indexed container layout:
      //   object:  [tag][value_data][hash[N]][slot[N]][count u16]
      //   array:   [tag][value_data][slot[N]][count u16]
      //
      // Index lives at the END of the container; readers walk backward
      // from container_end (provided by parent's slot or by the
      // top-level caller's buffer length).
      //
      // Single-pass forward encoding: write tag, stream value_data,
      // append hash[N] (objects only), append slot[N], append count.
      // No memmove, no header reservation, no backpatch beyond the
      // SBO of (hash, offset, key_size) tuples we record while
      // walking children.
      // Generic container layout: [tag][width byte][value_data][...index...]
      // Width byte's low 2 bits = slot_w_code (0=u8, 1=u16, 2=u24, 3=u32);
      // bits 2..7 reserved (must be 0). Each container picks slot_w
      // independently from its own value_data size — recursive: a small
      // sub-object inside a large doc still uses slot_w=u8.
      inline std::size_t array_size(const pjson_array& a) noexcept
      {
         std::size_t  N  = a.size();
         std::size_t  vd = container_value_data_size_array(a);
         std::uint8_t code = width_code_for(vd);
         std::size_t  slot_w = width_bytes(code);
         return 1u /*tag*/ + 1u /*width*/ + vd + slot_w * N + 2u /*count*/;
      }
      inline std::size_t object_size(const pjson_object& o) noexcept
      {
         std::size_t  N  = o.size();
         std::size_t  vd = container_value_data_size_object(o);
         std::uint8_t code = width_code_for(vd);
         std::size_t  slot_w = width_bytes(code);
         // Object slot stride = slot_w + 1 (offset + key_size byte).
         return 1u + 1u + vd + N /*hash*/ + (slot_w + 1) * N + 2u;
      }

      inline std::size_t value_size(const pjson_value& v) noexcept
      {
         return std::visit(
             [](const auto& x) -> std::size_t {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, pjson_null>) return 1;
                else if constexpr (std::is_same_v<T, bool>) return 1;
                else if constexpr (std::is_same_v<T, std::int64_t>)
                   return int_size(x);
                else if constexpr (std::is_same_v<T, double>)
                {
                   if (!std::isfinite(x)) return 9;
                   pjson_number n   = pjson_number::from_double(x);
                   std::size_t  dec = number_size(n);
                   return dec < 9 ? dec : 9;
                }
                else if constexpr (std::is_same_v<T, pjson_number>)
                   return number_size(x);
                else if constexpr (std::is_same_v<T, std::string>)
                   return string_size(x);
                else if constexpr (std::is_same_v<T, pjson_bytes>)
                   return bytes_size(x);
                else if constexpr (std::is_same_v<T, pjson_array>)
                   return array_size(x);
                else if constexpr (std::is_same_v<T, pjson_object>)
                   return object_size(x);
             },
             v.v);
      }

      // ── encoding ──────────────────────────────────────────────────────
      // All `encode_*_at` helpers write at dst[pos..pos+N) and return N.

      inline std::size_t encode_value_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_value& v);

      inline std::size_t encode_int64_at(std::uint8_t* dst,
                                         std::size_t   pos,
                                         std::int64_t  i) noexcept
      {
         if (i >= 0 && i <= 15)
         {
            dst[pos] = static_cast<std::uint8_t>(
                (t_uint_inline << 4) | static_cast<std::uint8_t>(i));
            return 1;
         }
         std::uint64_t zz =
             (static_cast<std::uint64_t>(i) << 1) ^
             static_cast<std::uint64_t>(i >> 63);
         std::uint8_t bc = 1;
         std::uint64_t t = zz >> 8;
         while (t) { ++bc; t >>= 8; }
         dst[pos] = static_cast<std::uint8_t>((t_int << 4) | (bc - 1));
         std::memcpy(dst + pos + 1, &zz, bc);
         return 1u + bc;
      }
      inline std::size_t encode_number_at(std::uint8_t*       dst,
                                          std::size_t         pos,
                                          const pjson_number& n) noexcept
      {
         if (n.scale == 0 && n.mantissa >= 0 && n.mantissa <= 15)
         {
            dst[pos] = static_cast<std::uint8_t>(
                (t_uint_inline << 4) | static_cast<std::uint8_t>(n.mantissa));
            return 1;
         }
         __uint128_t  zz = zz128_encode(n.mantissa);
         std::uint8_t bc = mantissa_byte_count(zz);
         if (n.scale == 0)
         {
            dst[pos] = static_cast<std::uint8_t>((t_int << 4) | (bc - 1));
            std::memcpy(dst + pos + 1, &zz, bc);
            return 1u + bc;
         }
         dst[pos] = static_cast<std::uint8_t>((t_decimal << 4) | (bc - 1));
         std::memcpy(dst + pos + 1, &zz, bc);
         std::size_t scale_bytes =
             write_varint62(dst, pos + 1 + bc, n.scale);
         return 1u + bc + scale_bytes;
      }
      inline std::size_t encode_double_raw_at(std::uint8_t* dst,
                                              std::size_t   pos,
                                              double        d) noexcept
      {
         dst[pos] = static_cast<std::uint8_t>(t_ieee_float << 4);
         std::memcpy(dst + pos + 1, &d, 8);
         return 9;
      }
      inline std::size_t encode_double_at(std::uint8_t* dst,
                                          std::size_t   pos,
                                          double        d)
      {
         if (!std::isfinite(d)) return encode_double_raw_at(dst, pos, d);
         pjson_number n   = pjson_number::from_double(d);
         std::size_t  dec = number_size(n);
         if (dec < 9) return encode_number_at(dst, pos, n);
         return encode_double_raw_at(dst, pos, d);
      }
      // Encode a text string. Default flag is raw_text (caller didn't
      // pre-escape the bytes; JSON emit will run a per-char escape).
      // Callers that already have escape-form bytes (e.g., from
      // simdjson's escaped_key()) should pass flag=escape_form to skip
      // emitter work later.
      inline std::size_t encode_string_at(std::uint8_t*    dst,
                                          std::size_t      pos,
                                          std::string_view s,
                                          std::uint8_t     flag =
                                              string_flag_raw_text) noexcept
      {
         dst[pos] = static_cast<std::uint8_t>((t_string << 4) | flag);
         if (!s.empty()) std::memcpy(dst + pos + 1, s.data(), s.size());
         return 1u + s.size();
      }
      // Encode raw binary bytes. JSON emit will base64-encode.
      inline std::size_t encode_bytes_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_bytes& b) noexcept
      {
         dst[pos] = static_cast<std::uint8_t>(t_bytes << 4);
         if (!b.empty()) std::memcpy(dst + pos + 1, b.data(), b.size());
         return 1u + b.size();
      }

      // Map a C++ element type to its tac_* code at compile time.
      template <typename T>
      inline constexpr std::uint8_t tac_for() noexcept
      {
         if constexpr (std::is_same_v<T, std::int8_t>)        return tac_i8;
         else if constexpr (std::is_same_v<T, std::int16_t>)  return tac_i16;
         else if constexpr (std::is_same_v<T, std::int32_t>)  return tac_i32;
         else if constexpr (std::is_same_v<T, std::int64_t>)  return tac_i64;
         else if constexpr (std::is_same_v<T, std::uint8_t>)  return tac_u8;
         else if constexpr (std::is_same_v<T, std::uint16_t>) return tac_u16;
         else if constexpr (std::is_same_v<T, std::uint32_t>) return tac_u32;
         else if constexpr (std::is_same_v<T, std::uint64_t>) return tac_u64;
         else if constexpr (std::is_same_v<T, float>)         return tac_f32;
         else if constexpr (std::is_same_v<T, double>)        return tac_f64;
         else
            static_assert(sizeof(T) == 0,
                          "encode_typed_array: unsupported element type");
      }

      // Total byte size of a typed_array carrying N elements of code.
      inline std::size_t typed_array_size(std::uint8_t code,
                                          std::size_t  N) noexcept
      {
         return 1u + typed_array_elem_size(code) * N + 2u;
      }

      // Tail-indexed typed-array encoder. Layout:
      //
      //   [tag = (t_array << 4) | (code + 1)]
      //   [N × element bytes, little-endian]
      //   [count: u16 LE]
      //
      // Low_nibble = code + 1 because low_nibble = 0 is the generic-
      // array marker. Total bytes = 1 + N*elem_size + 2.
      // Caller pre-sizes dst.
      template <typename T>
      inline std::size_t encode_typed_array_at(
          std::uint8_t*       dst,
          std::size_t         pos,
          std::span<const T>  elems) noexcept
      {
         constexpr std::uint8_t code = tac_for<T>();
         constexpr std::size_t  es   = sizeof(T);
         std::size_t            N    = elems.size();
         dst[pos] = static_cast<std::uint8_t>(
             (t_array << 4) | typed_array_low_from_code(code));
         if (N) std::memcpy(dst + pos + 1, elems.data(), N * es);
         std::size_t count_pos = pos + 1 + N * es;
         dst[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         return 1u + N * es + 2u;
      }

      // Tail-indexed array encoder.
      //
      // Layout:  [tag][value_data][slot[N]][count u16]
      //
      // We pre-compute value_data size from container_value_data_size_*
      // so we can place the slot table and count at known absolute
      // positions in the pre-sized output buffer. Children are written
      // forward into value_data; slot[i] is filled directly during the
      // walk. Count is written last.
      inline std::size_t encode_array_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_array& a)
      {
         std::size_t start = pos;
         dst[pos++]        = static_cast<std::uint8_t>(t_array << 4);
         std::size_t  N    = a.size();
         std::size_t  vd   = container_value_data_size_array(a);
         std::uint8_t slot_w_code = width_code_for(vd);
         std::size_t  slot_w      = width_bytes(slot_w_code);
         dst[pos++] = slot_w_code;  // width byte
         std::size_t value_data_start = pos;
         std::size_t slot_table_pos   = value_data_start + vd;
         std::size_t count_pos        = slot_table_pos + slot_w * N;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t off =
                static_cast<std::uint32_t>(pos - value_data_start);
            write_width(dst + slot_table_pos + i * slot_w,
                        slot_w_code, off);
            pos += encode_value_at(dst, pos, a[i]);
         }
         dst[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         return count_pos + 2 - start;
      }

      // Tail-indexed object encoder.
      //
      // Layout:  [tag][value_data][hash[N]][slot[N]][count u16]
      //
      // value_data per field: [key bytes (key_size)][tag][raw bits]
      //   or with long-key escape: [varuint excess][key bytes][tag][bits]

      // Object t_object low-nibble layout selector.
      enum : std::uint8_t
      {
         object_form_single    = 0,  // §5.2 — generic object
         object_form_row_array = 1,  // §5.2.1 — homogeneous row_array
         // 2..15 reserved
      };

      // True when every element is a pjson_object whose keys (in order,
      // byte-equal) match the first object's keys. Used by the writer
      // to choose row_array form vs. generic array of objects.
      inline bool array_is_row_array_homogeneous(
          const pjson_array& a) noexcept
      {
         if (a.empty()) return false;
         if (!a[0].holds<pjson_object>()) return false;
         const auto& first = a[0].as<pjson_object>();
         std::size_t K = first.size();
         for (std::size_t i = 1; i < a.size(); ++i)
         {
            if (!a[i].holds<pjson_object>()) return false;
            const auto& o = a[i].as<pjson_object>();
            if (o.size() != K) return false;
            for (std::size_t j = 0; j < K; ++j)
               if (o[j].first != first[j].first) return false;
         }
         return true;
      }

      // Encode a row_array. Caller must have already verified the
      // input is homogeneous (see array_is_row_array_homogeneous).
      // Two-pass:
      //   1. compute record body sizes + width selectors
      //   2. write tag, width byte, schema, records, indexes
      inline std::size_t encode_row_array_at(std::uint8_t*      dst,
                                             std::size_t        pos,
                                             const pjson_array& a)
      {
         std::size_t start    = pos;
         std::size_t N        = a.size();
         const auto& first    = a[0].as<pjson_object>();
         std::size_t K        = first.size();

         // Pass 1: per-record value_data sizes and per-field offsets.
         // Stored as a flat vector of N×K offsets to avoid per-record
         // heap allocs; record_value_sizes[i] is the i-th record's
         // total value_data size.
         std::vector<std::uint32_t> per_field_offset(N * K);
         std::vector<std::uint32_t> record_value_sizes(N);
         std::uint32_t              max_record_body = 0;
         for (std::size_t i = 0; i < N; ++i)
         {
            const auto&   obj  = a[i].as<pjson_object>();
            std::uint32_t off  = 0;
            for (std::size_t j = 0; j < K; ++j)
            {
               per_field_offset[i * K + j] = off;
               off += static_cast<std::uint32_t>(value_size(obj[j].second));
            }
            record_value_sizes[i] = off;  // body before per-record slot tail
         }

         // Compute per-record full body size (value_data + slot table)
         // and the running record_offset for each record.
         std::vector<std::uint32_t> record_offsets(N);
         std::uint32_t running = 0;
         std::uint8_t  slot_w_code; // determined after we know max body
         // Find slot_w first based on max record body — slot offsets
         // are within a record, bounded by record body size.
         for (std::size_t i = 0; i < N; ++i)
            max_record_body = std::max(max_record_body,
                                       record_value_sizes[i]);
         slot_w_code = width_code_for(max_record_body);
         std::size_t slot_w = width_bytes(slot_w_code);
         for (std::size_t i = 0; i < N; ++i)
         {
            record_offsets[i] = running;
            running += record_value_sizes[i] +
                       static_cast<std::uint32_t>(K * slot_w);
         }
         std::uint8_t recoff_w_code = width_code_for(running);
         std::size_t recoff_w = width_bytes(recoff_w_code);

         // Compute total keys area size + write shared schema.
         std::uint32_t keys_area_size = 0;
         for (std::size_t j = 0; j < K; ++j)
            keys_area_size += static_cast<std::uint32_t>(
                first[j].first.size());

         // Layout positions, written front-to-back:
         //   tag(1) width(1) K(varuint) keyslots(4K) hash(K) keys(...)
         //   then records body, then record_offsets[N], then count u16.
         dst[pos++] = static_cast<std::uint8_t>(
             (t_object << 4) | object_form_row_array);
         dst[pos++] = static_cast<std::uint8_t>(
             slot_w_code | (recoff_w_code << 2));
         pos += write_varuint62(dst, pos, K);

         std::size_t key_slots_pos = pos;
         pos += 4 * K;

         std::size_t hash_pos = pos;
         pos += K;

         std::size_t keys_pos = pos;
         std::uint32_t key_off_running = 0;
         for (std::size_t j = 0; j < K; ++j)
         {
            const auto&  k     = first[j].first;
            std::uint8_t ks_byte =
                k.size() < 0xFFu ? static_cast<std::uint8_t>(k.size())
                                 : static_cast<std::uint8_t>(0xFFu);
            write_u32_le(dst + key_slots_pos + j * 4,
                         pack_slot(key_off_running, ks_byte));
            dst[hash_pos + j] = key_hash8(k);
            std::memcpy(dst + pos, k.data(), k.size());
            pos += k.size();
            key_off_running += static_cast<std::uint32_t>(k.size());
            // Long-key escape (key_size == 0xFF) is supported but
            // requires a varuint excess inside the keys area; skip
            // for now since canonical encoders limit key length to
            // < 255 (see §5.4).
         }
         (void)keys_pos;

         // Records body.
         std::size_t records_body_start = pos;
         for (std::size_t i = 0; i < N; ++i)
         {
            const auto& obj            = a[i].as<pjson_object>();
            std::size_t record_start    = pos;
            std::size_t record_value_sz = record_value_sizes[i];
            for (std::size_t j = 0; j < K; ++j)
               pos += encode_value_at(dst, pos, obj[j].second);
            // Per-record slot table at the tail of the record.
            std::size_t slot_pos = record_start + record_value_sz;
            for (std::size_t j = 0; j < K; ++j)
               write_width(dst + slot_pos + j * slot_w, slot_w_code,
                           per_field_offset[i * K + j]);
            pos = slot_pos + K * slot_w;
         }

         // record_offsets[N] tail-indexed.
         std::size_t record_offsets_pos = pos;
         for (std::size_t i = 0; i < N; ++i)
            write_width(dst + record_offsets_pos + i * recoff_w,
                        recoff_w_code, record_offsets[i]);
         pos += N * recoff_w;
         (void)records_body_start;

         // count u16 LE.
         dst[pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         pos += 2;

         return pos - start;
      }

      // Pre-compute the byte size of a row_array encoding for `a`.
      // Mirrors encode_row_array_at's pass 1 + size accounting; used
      // by the public encoder to size the output buffer up front.
      inline std::size_t row_array_size(const pjson_array& a) noexcept
      {
         std::size_t N = a.size();
         if (N == 0) return 0;
         const auto& first = a[0].as<pjson_object>();
         std::size_t K     = first.size();

         std::uint32_t max_body = 0;
         std::uint32_t running  = 0;
         std::vector<std::uint32_t> body_sizes(N);
         for (std::size_t i = 0; i < N; ++i)
         {
            const auto&   obj = a[i].as<pjson_object>();
            std::uint32_t b   = 0;
            for (std::size_t j = 0; j < K; ++j)
               b += static_cast<std::uint32_t>(value_size(obj[j].second));
            body_sizes[i] = b;
            max_body = std::max(max_body, b);
         }
         std::uint8_t slot_w_code = width_code_for(max_body);
         std::size_t  slot_w      = width_bytes(slot_w_code);
         for (std::size_t i = 0; i < N; ++i)
            running += body_sizes[i] +
                       static_cast<std::uint32_t>(K * slot_w);
         std::uint8_t recoff_w_code = width_code_for(running);
         std::size_t  recoff_w      = width_bytes(recoff_w_code);

         std::uint32_t keys_area = 0;
         for (std::size_t j = 0; j < K; ++j)
            keys_area += static_cast<std::uint32_t>(first[j].first.size());

         return 1 /*tag*/ + 1 /*width byte*/
              + varuint62_byte_count(K)
              + 4 * K /*shared key slots*/
              + K /*hash[K]*/
              + keys_area
              + running /*records body*/
              + N * recoff_w /*record_offsets*/
              + 2 /*count*/;
      }

      inline std::size_t encode_object_at(std::uint8_t*       dst,
                                          std::size_t         pos,
                                          const pjson_object& o)
      {
         std::size_t start = pos;
         dst[pos++]        = static_cast<std::uint8_t>(t_object << 4);
         std::size_t  N    = o.size();
         std::size_t  vd   = container_value_data_size_object(o);
         std::uint8_t slot_w_code = width_code_for(vd);
         std::size_t  slot_w      = width_bytes(slot_w_code);
         dst[pos++] = slot_w_code;  // width byte
         std::size_t value_data_start = pos;
         std::size_t hash_table_pos   = value_data_start + vd;
         std::size_t slot_table_pos   = hash_table_pos + N;
         std::size_t count_pos        = slot_table_pos + (slot_w + 1) * N;
         for (std::size_t i = 0; i < N; ++i)
         {
            const auto& [k, v] = o[i];
            std::uint32_t off  =
                static_cast<std::uint32_t>(pos - value_data_start);
            std::uint8_t ks_byte =
                k.size() < 0xFFu ? static_cast<std::uint8_t>(k.size())
                                 : static_cast<std::uint8_t>(0xFFu);
            std::uint8_t* slot = dst + slot_table_pos + i * (slot_w + 1);
            write_width(slot, slot_w_code, off);
            slot[slot_w] = ks_byte;
            dst[hash_table_pos + i] = key_hash8(k);
            if (k.size() >= 0xFFu)
               pos += write_varuint62(dst, pos, k.size() - 0xFFu);
            std::memcpy(dst + pos, k.data(), k.size());
            pos += k.size();
            pos += encode_value_at(dst, pos, v);
         }
         dst[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         return count_pos + 2 - start;
      }

      inline std::size_t encode_value_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_value& v)
      {
         return std::visit(
             [&](const auto& x) -> std::size_t {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, pjson_null>)
                {
                   dst[pos] = static_cast<std::uint8_t>(t_null << 4);
                   return 1;
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                   // tag = (t_bool << 4) | (x ? 1 : 0)
                   dst[pos] = static_cast<std::uint8_t>(
                       (t_bool << 4) | (x ? 1u : 0u));
                   return 1;
                }
                else if constexpr (std::is_same_v<T, std::int64_t>)
                   return encode_int64_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, double>)
                   return encode_double_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_number>)
                   return encode_number_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, std::string>)
                   return encode_string_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_bytes>)
                   return encode_bytes_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_array>)
                   return encode_array_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_object>)
                   return encode_object_at(dst, pos, x);
             },
             v.v);
      }

      // ── decoding (full materialize into pjson_value tree) ─────────────
      // The (ptr, size) pair fully describes the value.

      inline bool decode_value(const std::uint8_t* p,
                               std::size_t         size,
                               pjson_value&        out);

      // Tail-indexed array decoder. Reads count from the last 2 bytes,
      // then computes index positions backward.
      inline bool decode_array(const std::uint8_t* p,
                               std::size_t         size,
                               pjson_value&        out)
      {
         if (size < 4) return false;  // tag + width + count u16 minimum
         std::uint8_t  width_byte  = p[1];
         if (width_byte & 0xFCu) return false;  // reserved bits
         std::uint8_t  slot_w_code = width_byte & 0x03;
         std::size_t   slot_w      = width_bytes(slot_w_code);
         std::uint16_t N = static_cast<std::uint16_t>(p[size - 2]) |
                           (static_cast<std::uint16_t>(p[size - 1]) << 8);
         std::size_t slot_table_pos = size - 2 - slot_w * N;
         if (slot_table_pos < 2 || slot_table_pos > size - 2)
            return false;  // overflow / impossible layout
         std::size_t value_data_start = 2;
         std::size_t value_data_size  = slot_table_pos - value_data_start;

         pjson_array a;
         a.reserve(N);
         for (std::uint16_t i = 0; i < N; ++i)
         {
            std::uint32_t off_i =
                read_width(p + slot_table_pos + i * slot_w, slot_w_code);
            std::uint32_t off_next =
                i + 1 < N
                    ? read_width(p + slot_table_pos + (i + 1) * slot_w,
                                 slot_w_code)
                    : static_cast<std::uint32_t>(value_data_size);
            if (off_next < off_i || off_next > value_data_size)
               return false;
            pjson_value child;
            if (!decode_value(p + value_data_start + off_i,
                              off_next - off_i, child))
               return false;
            a.push_back(std::move(child));
         }
         out = pjson_value{std::move(a)};
         return true;
      }

      // Row-array decoder. Reads the shared schema, then for each
      // record reconstructs a pjson_object with the shared keys + the
      // record's per-field values. Output: pjson_value{pjson_array} of
      // pjson_value{pjson_object}.
      inline bool decode_row_array(const std::uint8_t* p,
                                   std::size_t         size,
                                   pjson_value&        out)
      {
         if (size < 4) return false;  // tag + width + K + count u16 minimum
         std::uint8_t width_byte    = p[1];
         std::uint8_t slot_w_code   = width_byte & 0x03;
         std::uint8_t recoff_w_code = (width_byte >> 2) & 0x03;
         std::size_t  slot_w        = width_bytes(slot_w_code);
         std::size_t  recoff_w      = width_bytes(recoff_w_code);

         std::uint64_t K_u64;
         std::size_t   K_bytes =
             read_varuint62(p + 2, size - 2, K_u64);
         if (K_bytes == 0) return false;
         std::size_t K = static_cast<std::size_t>(K_u64);
         if (K == 0) return false;

         std::size_t key_slots_pos = 2 + K_bytes;
         std::size_t hash_pos      = key_slots_pos + 4 * K;
         std::size_t keys_pos      = hash_pos + K;
         if (keys_pos > size - 2) return false;

         // Compute keys area total size from the last key slot.
         std::uint32_t last_slot = read_u32_le(
             p + key_slots_pos + (K - 1) * 4);
         std::uint32_t last_off  = slot_offset(last_slot);
         std::uint8_t  last_ks   = slot_key_size(last_slot);
         if (last_ks == 0xFF) return false;  // long keys not supported in row_array yet
         std::uint32_t keys_area_size = last_off + last_ks;
         std::size_t   records_body_start = keys_pos + keys_area_size;

         std::uint16_t N = static_cast<std::uint16_t>(p[size - 2]) |
                           (static_cast<std::uint16_t>(p[size - 1]) << 8);
         std::size_t record_offsets_pos = size - 2 - N * recoff_w;
         if (record_offsets_pos < records_body_start) return false;
         std::size_t records_body_size =
             record_offsets_pos - records_body_start;

         // Build shared key list.
         std::vector<std::string_view> keys(K);
         for (std::size_t j = 0; j < K; ++j)
         {
            std::uint32_t s_j = read_u32_le(p + key_slots_pos + j * 4);
            std::uint32_t kof = slot_offset(s_j);
            std::uint8_t  ksz = slot_key_size(s_j);
            if (ksz == 0xFF) return false;
            keys[j] = std::string_view(
                reinterpret_cast<const char*>(p + keys_pos + kof), ksz);
            if (p[hash_pos + j] != key_hash8(keys[j])) return false;
         }

         pjson_array out_arr;
         out_arr.reserve(N);
         for (std::uint16_t i = 0; i < N; ++i)
         {
            std::uint32_t roff_i =
                read_width(p + record_offsets_pos + i * recoff_w,
                           recoff_w_code);
            std::uint32_t roff_next =
                i + 1 < N
                    ? read_width(p + record_offsets_pos +
                                     (i + 1) * recoff_w,
                                 recoff_w_code)
                    : static_cast<std::uint32_t>(records_body_size);
            if (roff_next < roff_i) return false;
            std::size_t  rec_size  = roff_next - roff_i;
            const std::uint8_t* rec = p + records_body_start + roff_i;
            if (K * slot_w > rec_size) return false;
            std::size_t  slot_pos  = rec_size - K * slot_w;
            std::size_t  body_size = slot_pos;

            pjson_object obj;
            obj.reserve(K);
            for (std::size_t j = 0; j < K; ++j)
            {
               std::uint32_t voff =
                   read_width(rec + slot_pos + j * slot_w, slot_w_code);
               std::uint32_t vend =
                   j + 1 < K
                       ? read_width(rec + slot_pos + (j + 1) * slot_w,
                                    slot_w_code)
                       : static_cast<std::uint32_t>(body_size);
               if (vend < voff || vend > body_size) return false;
               pjson_value child;
               if (!decode_value(rec + voff, vend - voff, child))
                  return false;
               obj.emplace_back(std::string(keys[j]), std::move(child));
            }
            out_arr.push_back(pjson_value{std::move(obj)});
         }
         out = pjson_value{std::move(out_arr)};
         return true;
      }

      // Tail-indexed object decoder.
      inline bool decode_object(const std::uint8_t* p,
                                std::size_t         size,
                                pjson_value&        out)
      {
         if (size < 4) return false;  // tag + width + count u16 minimum
         std::uint8_t  width_byte  = p[1];
         if (width_byte & 0xFCu) return false;
         std::uint8_t  slot_w_code = width_byte & 0x03;
         std::size_t   slot_w      = width_bytes(slot_w_code);
         std::size_t   slot_stride = slot_w + 1;  // offset + key_size byte
         std::uint16_t N = static_cast<std::uint16_t>(p[size - 2]) |
                           (static_cast<std::uint16_t>(p[size - 1]) << 8);
         std::size_t slot_table_pos = size - 2 - slot_stride * N;
         std::size_t hash_table_pos = slot_table_pos - N;
         if (slot_table_pos < 2 || slot_table_pos > size - 2 ||
             hash_table_pos < 2 || hash_table_pos > slot_table_pos)
            return false;
         std::size_t value_data_start = 2;
         std::size_t value_data_size  = hash_table_pos - value_data_start;

         pjson_object o;
         o.reserve(N);
         for (std::uint16_t i = 0; i < N; ++i)
         {
            const std::uint8_t* slot =
                p + slot_table_pos + i * slot_stride;
            std::uint32_t off_i = read_width(slot, slot_w_code);
            std::uint8_t  ks    = slot[slot_w];
            std::uint32_t off_next =
                i + 1 < N
                    ? read_width(p + slot_table_pos +
                                     (i + 1) * slot_stride,
                                 slot_w_code)
                    : static_cast<std::uint32_t>(value_data_size);
            if (off_next < off_i || off_next > value_data_size)
               return false;
            const std::uint8_t* entry      = p + value_data_start + off_i;
            std::size_t         entry_size = off_next - off_i;
            std::size_t         klen_bytes;
            std::size_t         klen;
            if (ks != 0xFF)
            {
               klen       = ks;
               klen_bytes = 0;
            }
            else
            {
               std::uint64_t excess;
               klen_bytes =
                   read_varuint62(entry, entry_size, excess);
               if (klen_bytes == 0) return false;
               klen = 0xFFu + static_cast<std::size_t>(excess);
            }
            if (klen_bytes + klen > entry_size) return false;
            std::string_view k(
                reinterpret_cast<const char*>(entry + klen_bytes), klen);
            if (p[hash_table_pos + i] != key_hash8(k)) return false;
            pjson_value child;
            std::size_t value_off = klen_bytes + klen;
            if (!decode_value(entry + value_off,
                              entry_size - value_off, child))
               return false;
            o.emplace_back(std::string(k), std::move(child));
         }
         out = pjson_value{std::move(o)};
         return true;
      }

      inline bool decode_value(const std::uint8_t* p,
                               std::size_t         size,
                               pjson_value&        out)
      {
         if (size == 0) return false;
         std::uint8_t tag  = p[0];
         std::uint8_t type = tag >> 4;
         std::uint8_t low  = tag & 0x0F;

         switch (type)
         {
            case t_null: out = pjson_value{pjson_null{}}; return size == 1;
            case t_bool:
               // low nibble holds the value: 0 = false, 1 = true.
               // Anything else is an error.
               if (size != 1 || low > 1) return false;
               out = pjson_value{low == 1};
               return true;
            case t_uint_inline:
               out = pjson_value{static_cast<std::int64_t>(low)};
               return size == 1;
            case t_int:
            {
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               if (1u + bc != size) return false;
               if (bc <= 8)
               {
                  std::uint64_t zz = 0;
                  std::memcpy(&zz, p + 1, bc);
                  out = pjson_value{static_cast<std::int64_t>(
                      (zz >> 1) ^ (~(zz & 1) + 1))};
               }
               else
               {
                  __uint128_t zz = 0;
                  std::memcpy(&zz, p + 1, bc);
                  out = pjson_value{
                      pjson_number{zz128_decode(zz), 0}};
               }
               return true;
            }
            case t_decimal:
            {
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               if (1u + bc > size) return false;
               __uint128_t zz = 0;
               std::memcpy(&zz, p + 1, bc);
               std::int32_t scale;
               std::size_t  scale_bytes =
                   read_varint62(p + 1 + bc, size - 1 - bc, scale);
               if (scale_bytes == 0) return false;
               if (1u + bc + scale_bytes != size) return false;
               pjson_number n{zz128_decode(zz), scale};
               // Variant normalize: tag 5 → double when value fits exactly.
               __int128 m_abs =
                   n.mantissa < 0 ? -n.mantissa : n.mantissa;
               if (m_abs < (static_cast<__int128>(1) << 53) &&
                   scale >= -22 && scale <= 22)
               {
                  out = pjson_value{n.to_double()};
                  return true;
               }
               double d = n.to_double();
               if (std::isfinite(d))
               {
                  pjson_number rt = pjson_number::from_double(d);
                  if (rt == n)
                  {
                     out = pjson_value{d};
                     return true;
                  }
               }
               out = pjson_value{n};
               return true;
            }
            case t_ieee_float:
            {
               if (size != 9) return false;
               double d;
               std::memcpy(&d, p + 1, 8);
               out = pjson_value{d};
               return true;
            }
            case t_string:
            {
               // Low-nibble flag tells us whether the bytes are
               // raw_text (0) or escape_form (1). Reject reserved
               // flag values.
               if (low > string_flag_escape_form) return false;
               out = pjson_value{std::string(
                   reinterpret_cast<const char*>(p + 1), size - 1)};
               return true;
            }
            case t_bytes:
            {
               // Tag's low nibble must be 0; raw bytes follow.
               if (low != 0) return false;
               out = pjson_value{pjson_bytes(p + 1, p + size)};
               return true;
            }
            case t_array:
            {
               if (low == 0) return decode_array(p, size, out);
               // Typed array: low nibble = code + 1.
               std::uint8_t code = typed_array_code_from_low(low);
               if (code == tac_invalid) return false;
               std::size_t es = typed_array_elem_size(code);
               if (es == 0 || size < 3) return false;
               std::uint16_t N =
                   static_cast<std::uint16_t>(p[size - 2]) |
                   (static_cast<std::uint16_t>(p[size - 1]) << 8);
               if (1u + es * static_cast<std::size_t>(N) + 2u != size)
                  return false;
               pjson_array a;
               a.reserve(N);
               const std::uint8_t* base = p + 1;
               for (std::uint16_t i = 0; i < N; ++i)
               {
                  const std::uint8_t* eb = base + i * es;
                  switch (code)
                  {
                     case tac_i8:
                     {
                        std::int8_t v;
                        std::memcpy(&v, eb, 1);
                        a.push_back(pjson_value{
                            static_cast<std::int64_t>(v)});
                        break;
                     }
                     case tac_i16:
                     {
                        std::int16_t v;
                        std::memcpy(&v, eb, 2);
                        a.push_back(pjson_value{
                            static_cast<std::int64_t>(v)});
                        break;
                     }
                     case tac_i32:
                     {
                        std::int32_t v;
                        std::memcpy(&v, eb, 4);
                        a.push_back(pjson_value{
                            static_cast<std::int64_t>(v)});
                        break;
                     }
                     case tac_i64:
                     {
                        std::int64_t v;
                        std::memcpy(&v, eb, 8);
                        a.push_back(pjson_value{v});
                        break;
                     }
                     case tac_u8:
                        a.push_back(pjson_value{
                            static_cast<std::int64_t>(eb[0])});
                        break;
                     case tac_u16:
                     {
                        std::uint16_t v;
                        std::memcpy(&v, eb, 2);
                        a.push_back(pjson_value{
                            static_cast<std::int64_t>(v)});
                        break;
                     }
                     case tac_u32:
                     {
                        std::uint32_t v;
                        std::memcpy(&v, eb, 4);
                        a.push_back(pjson_value{
                            static_cast<std::int64_t>(v)});
                        break;
                     }
                     case tac_u64:
                     {
                        std::uint64_t v;
                        std::memcpy(&v, eb, 8);
                        // u64 may exceed i64 range; promote to
                        // pjson_number when needed.
                        if (v <= static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max()))
                           a.push_back(pjson_value{
                               static_cast<std::int64_t>(v)});
                        else
                           a.push_back(pjson_value{pjson_number{
                               static_cast<__int128>(v), 0}});
                        break;
                     }
                     case tac_f32:
                     {
                        float v;
                        std::memcpy(&v, eb, 4);
                        a.push_back(pjson_value{
                            static_cast<double>(v)});
                        break;
                     }
                     case tac_f64:
                     {
                        double v;
                        std::memcpy(&v, eb, 8);
                        a.push_back(pjson_value{v});
                        break;
                     }
                     default: return false;
                  }
               }
               out = pjson_value{std::move(a)};
               return true;
            }
            case t_object:
               if (low == object_form_single)    return decode_object(p, size, out);
               if (low == object_form_row_array) return decode_row_array(p, size, out);
               return false;  // reserved low_nibble
            default:       return false;
         }
      }

   }  // namespace pjson_detail

   // ── pjson_number out-of-line definitions ─────────────────────────────

   inline pjson_number pjson_number::from_double(double d)
   {
      if (!std::isfinite(d))
         throw std::invalid_argument("pjson: NaN/Inf not representable");
      char buf[32];
      auto r = std::to_chars(buf, buf + sizeof(buf), d);
      if (r.ec != std::errc{})
         throw std::runtime_error("pjson: to_chars failed");
      std::string_view s(buf, r.ptr - buf);
      bool        neg = false;
      std::size_t i   = 0;
      if (i < s.size() && s[i] == '-') { neg = true; ++i; }
      __int128     mant     = 0;
      std::int32_t scale    = 0;
      bool         seen_dot = false;
      for (; i < s.size(); ++i)
      {
         char c = s[i];
         if (c == '.') { seen_dot = true; continue; }
         if (c == 'e' || c == 'E')
         {
            ++i;
            int sign = 1;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            {
               if (s[i] == '-') sign = -1;
               ++i;
            }
            std::int32_t exp = 0;
            for (; i < s.size(); ++i) exp = exp * 10 + (s[i] - '0');
            scale += sign * exp;
            break;
         }
         mant = mant * 10 + (c - '0');
         if (seen_dot) --scale;
      }
      if (neg) mant = -mant;
      return pjson_number{mant, scale};
   }

   inline pjson_number pjson_number::from_string(std::string_view s)
   {
      std::size_t i = 0, n = s.size();
      auto skip_ws = [&] {
         while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                          s[i] == '\r'))
            ++i;
      };
      skip_ws();
      bool neg = false;
      if (i < n && s[i] == '-') { neg = true; ++i; }
      if (i >= n || s[i] < '0' || s[i] > '9')
         throw std::invalid_argument("pjson: expected digit");
      __int128     mant     = 0;
      std::int32_t scale    = 0;
      bool         seen_dot = false;
      while (i < n)
      {
         char c = s[i];
         if (c == '.')
         {
            if (seen_dot)
               throw std::invalid_argument("pjson: stray '.'");
            seen_dot = true;
            ++i;
            continue;
         }
         if (c == 'e' || c == 'E')
         {
            ++i;
            int sign = 1;
            if (i < n && (s[i] == '+' || s[i] == '-'))
            {
               if (s[i] == '-') sign = -1;
               ++i;
            }
            std::int32_t exp = 0;
            if (i >= n || s[i] < '0' || s[i] > '9')
               throw std::invalid_argument("pjson: bad exponent");
            while (i < n && s[i] >= '0' && s[i] <= '9')
            {
               exp = exp * 10 + (s[i] - '0');
               ++i;
            }
            scale += sign * exp;
            break;
         }
         if (c < '0' || c > '9') break;
         mant = mant * 10 + (c - '0');
         if (seen_dot) --scale;
         ++i;
      }
      skip_ws();
      if (i != n)
         throw std::invalid_argument("pjson: trailing garbage");
      if (mant != 0)
      {
         while (scale < 0 && (mant % 10) == 0)
         {
            mant /= 10;
            ++scale;
         }
      }
      if (neg) mant = -mant;
      return pjson_number{mant, scale};
   }

   // ── public façade ────────────────────────────────────────────────────

   struct pjson
   {
      static std::vector<std::uint8_t> encode(const pjson_value& v)
      {
         std::size_t total = pjson_detail::value_size(v);
         std::vector<std::uint8_t> out(total);
         pjson_detail::encode_value_at(out.data(), 0, v);
         return out;
      }

      static std::size_t encoded_size(const pjson_value& v) noexcept
      {
         return pjson_detail::value_size(v);
      }

      static std::optional<pjson_value>
      try_decode(std::span<const std::uint8_t> bytes)
      {
         pjson_value v;
         if (!pjson_detail::decode_value(bytes.data(), bytes.size(), v))
            return std::nullopt;
         return v;
      }
      static pjson_value decode(std::span<const std::uint8_t> bytes)
      {
         auto v = try_decode(bytes);
         if (!v)
            throw std::runtime_error("pjson: invalid buffer");
         return std::move(*v);
      }
      static bool validate(std::span<const std::uint8_t> bytes) noexcept
      {
         pjson_value v;
         return pjson_detail::decode_value(bytes.data(), bytes.size(), v);
      }
   };

}  // namespace psio
