#pragma once
//
// psio/ext_int.hpp — 128-bit and 256-bit integer types.
//
// Byte-compatible with psio1::uint128 / psio1::int128 / psio1::uint256 in v1.
// Used for SSZ/Ethereum-consensus workloads (balances, hashes, KZG).
// Wire form is always 16 or 32 raw LE bytes; arithmetic is out of scope
// (users convert to their preferred bignum library via the limb array).
//
// Text representation: `0x`-prefixed lowercase hex with no leading
// zeros except for the zero value itself (`"0x0"`). Signed types use a
// leading `-` for negatives (signed-magnitude, NOT 2's complement
// hex). Registered as the default `text_category` adapter at the
// bottom of this header — that's the form JSON, WIT, and textual
// schemas emit and consume. Binary formats keep their existing
// little-endian wire form via per-format dispatch.

#include <psio/adapter.hpp>
#include <psio/error.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio {

   // Native compiler-extension aliases — sizeof == 16, 16-byte aligned.
   using uint128 = unsigned __int128;
   using int128  = __int128;

   // 32-byte LE unsigned, four u64 limbs (limb[0] = least-significant).
   struct uint256
   {
      std::uint64_t limb[4]{};

      constexpr uint256() noexcept = default;
      constexpr explicit uint256(std::uint64_t v) noexcept
         : limb{v, 0, 0, 0}
      {
      }
      constexpr explicit uint256(uint128 v) noexcept
         : limb{static_cast<std::uint64_t>(v),
                static_cast<std::uint64_t>(v >> 64),
                0,
                0}
      {
      }

      bool operator==(const uint256&) const noexcept = default;
      auto operator<=>(const uint256&) const noexcept = default;
   };

   static_assert(sizeof(uint256) == 32, "uint256 must be exactly 32 bytes");
   static_assert(alignof(uint256) == alignof(std::uint64_t));
   static_assert(std::is_standard_layout_v<uint256>);
   static_assert(std::is_trivially_copyable_v<uint256>);

   // 16-byte IEEE 754 binary128. Layout matches the Berkeley softfloat
   // `float128_t { uint64_t v[2]; }` — limb[0] = low 64 bits, limb[1]
   // = high 64 bits (low limb at the lower address on little-endian
   // hosts). Arithmetic is out of scope; this exists so `float128`
   // values can flow through a psio table key, a pssz row, or a JSON
   // payload without losing bits. Spring's `index_long_double_index`
   // is the canonical consumer — bridge softfloat's `float128_t` to
   // `psio::float128` with a delegate adapter or by memcpy.
   //
   // Sort order: psio::key's adapter applies the standard IEEE 754
   // sort transform (sign-flip-or-bitnot, then big-endian) so memcmp
   // matches IEEE less-than for finite, non-NaN values, including
   // -0.0 == +0.0. NaN comparison stays unordered as the standard
   // requires.
   struct float128
   {
      std::uint64_t limb[2]{};

      bool operator==(const float128&) const noexcept = default;
      auto operator<=>(const float128&) const noexcept = default;
   };

   static_assert(sizeof(float128) == 16, "float128 must be exactly 16 bytes");
   static_assert(alignof(float128) == alignof(std::uint64_t));
   static_assert(std::is_standard_layout_v<float128>);
   static_assert(std::is_trivially_copyable_v<float128>);

   // ── Hex helpers (used by the text adapters below) ───────────────────

   namespace ext_int_detail {

      inline constexpr char hex_digit(unsigned v) noexcept
      {
         return static_cast<char>(v < 10 ? ('0' + v) : ('a' + v - 10));
      }

      inline int hex_value(char c) noexcept
      {
         if (c >= '0' && c <= '9') return c - '0';
         if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
         if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
         return -1;
      }

      // Append `v` as minimal-width lowercase hex (no "0x" prefix).
      // For v == 0 emits a single '0'.
      inline void append_hex_u64(std::string& s, std::uint64_t v)
      {
         if (v == 0) { s.push_back('0'); return; }
         char  buf[16];
         int   n = 0;
         while (v && n < 16) { buf[n++] = hex_digit(v & 0xfu); v >>= 4; }
         while (n--) s.push_back(buf[n]);
      }

      // Append `v` (full 128 bits) as minimal-width hex.
      inline void append_hex_u128(std::string& s, uint128 v)
      {
         if (v == 0) { s.push_back('0'); return; }
         char buf[32];
         int  n = 0;
         while (v && n < 32) { buf[n++] = hex_digit(static_cast<unsigned>(v & 0xfu)); v >>= 4; }
         while (n--) s.push_back(buf[n]);
      }

      // Append uint256 (4 LE limbs) as minimal-width hex MSB-first.
      inline void append_hex_u256(std::string& s, const uint256& v)
      {
         // Find most-significant non-zero limb.
         int top = 3;
         while (top >= 0 && v.limb[top] == 0) --top;
         if (top < 0) { s.push_back('0'); return; }
         // First non-zero limb: minimal width.
         append_hex_u64(s, v.limb[top]);
         // Remaining limbs: full 16 hex chars each.
         for (int i = top - 1; i >= 0; --i)
         {
            std::uint64_t l = v.limb[i];
            for (int shift = 60; shift >= 0; shift -= 4)
               s.push_back(hex_digit((l >> shift) & 0xfu));
         }
      }

      // Parse a span of hex chars [p, end) into a uint128. Returns
      // codec_status::ok on success. Sets `out` only on success.
      inline codec_status parse_hex_u128(std::span<const char> hex,
                                         uint128&              out) noexcept
      {
         if (hex.empty() || hex.size() > 32)
            return codec_fail("ext_int hex: empty or too long", 0, "ext_int");
         uint128 acc = 0;
         for (char c : hex)
         {
            int d = hex_value(c);
            if (d < 0)
               return codec_fail("ext_int hex: invalid digit", 0, "ext_int");
            acc = (acc << 4) | static_cast<uint128>(d);
         }
         out = acc;
         return codec_ok();
      }

      // Parse hex into a uint256. Up to 64 hex chars.
      inline codec_status parse_hex_u256(std::span<const char> hex,
                                         uint256&              out) noexcept
      {
         if (hex.empty() || hex.size() > 64)
            return codec_fail("ext_int hex: empty or too long", 0, "ext_int");
         uint256 acc{};
         for (char c : hex)
         {
            int d = hex_value(c);
            if (d < 0)
               return codec_fail("ext_int hex: invalid digit", 0, "ext_int");
            // Shift the whole 256-bit value left by 4 bits.
            std::uint64_t carry = static_cast<std::uint64_t>(d);
            for (int i = 0; i < 4; ++i)
            {
               std::uint64_t lo  = acc.limb[i];
               std::uint64_t out_lo = (lo << 4) | carry;
               carry = lo >> 60;
               acc.limb[i] = out_lo;
            }
         }
         out = acc;
         return codec_ok();
      }

      // Helpers that strip the optional sign + "0x" framing and return
      // the body span. `negative_out` reports the parsed sign; for
      // unsigned variants the caller should reject a leading `-`.
      inline codec_status strip_framing(std::span<const char> in,
                                        bool& negative_out,
                                        std::span<const char>& body_out) noexcept
      {
         negative_out = false;
         if (in.size() >= 1 && in[0] == '-')
         {
            negative_out = true;
            in = std::span<const char>(in.data() + 1, in.size() - 1);
         }
         if (in.size() < 3 || in[0] != '0' || (in[1] != 'x' && in[1] != 'X'))
            return codec_fail("ext_int: expected 0x-prefixed hex", 0,
                              "ext_int");
         body_out = std::span<const char>(in.data() + 2, in.size() - 2);
         return codec_ok();
      }

   }  // namespace ext_int_detail

   // ── text_category adapters ──────────────────────────────────────────

   struct uint128_text_codec
   {
      static std::size_t packsize(const uint128& v) noexcept
      {
         std::string tmp;
         encode(v, tmp);
         return tmp.size();
      }

      static void encode(const uint128& v, std::string& s)
      {
         s += "0x";
         ext_int_detail::append_hex_u128(s, v);
      }

      static uint128 decode(std::span<const char> in)
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok() || neg)
            return uint128{0};  // Best-effort fallback; validate() catches it.
         uint128 out = 0;
         (void)ext_int_detail::parse_hex_u128(body, out);
         return out;
      }

      static codec_status validate(std::span<const char> in) noexcept
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok())
            return st;
         if (neg)
            return codec_fail("uint128: leading '-' not allowed", 0,
                              "ext_int");
         uint128 dummy = 0;
         return ext_int_detail::parse_hex_u128(body, dummy);
      }

      static codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   struct int128_text_codec
   {
      static std::size_t packsize(const int128& v) noexcept
      {
         std::string tmp;
         encode(v, tmp);
         return tmp.size();
      }

      static void encode(const int128& v, std::string& s)
      {
         uint128 mag;
         if (v < 0)
         {
            s.push_back('-');
            // Negate via two's-complement; equivalent to `-v` for any
            // value other than INT128_MIN, which still works because
            // the unsigned 2's-complement representation is correct.
            mag = static_cast<uint128>(-static_cast<int128>(v));
         }
         else
         {
            mag = static_cast<uint128>(v);
         }
         s += "0x";
         ext_int_detail::append_hex_u128(s, mag);
      }

      static int128 decode(std::span<const char> in)
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok())
            return 0;
         uint128 mag = 0;
         (void)ext_int_detail::parse_hex_u128(body, mag);
         return neg ? -static_cast<int128>(mag) : static_cast<int128>(mag);
      }

      static codec_status validate(std::span<const char> in) noexcept
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok())
            return st;
         uint128 dummy = 0;
         return ext_int_detail::parse_hex_u128(body, dummy);
      }

      static codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   struct uint256_text_codec
   {
      static std::size_t packsize(const uint256& v) noexcept
      {
         std::string tmp;
         encode(v, tmp);
         return tmp.size();
      }

      static void encode(const uint256& v, std::string& s)
      {
         s += "0x";
         ext_int_detail::append_hex_u256(s, v);
      }

      static uint256 decode(std::span<const char> in)
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok() || neg)
            return uint256{};
         uint256 out{};
         (void)ext_int_detail::parse_hex_u256(body, out);
         return out;
      }

      static codec_status validate(std::span<const char> in) noexcept
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok())
            return st;
         if (neg)
            return codec_fail("uint256: leading '-' not allowed", 0,
                              "ext_int");
         uint256 dummy{};
         return ext_int_detail::parse_hex_u256(body, dummy);
      }

      static codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   // ── float128 codecs ────────────────────────────────────────────────
   //
   // Three slots:
   //   binary_category         → raw 16 LE bytes (bin / pssz / frac / ssz)
   //   sortable_binary_category → sort-transformed BE (psio::key)
   //   text_category           → "0x" + 32 lowercase hex chars
   //
   // The wire and sort encodings differ — IEEE 754 binary128 stored
   // raw is NOT memcmp-sortable (sign bit is high, magnitude grows
   // upward only for positives) — so the two binary slots get
   // distinct codecs.

   struct float128_binary_codec
   {
      static std::size_t packsize(const float128&) noexcept { return 16; }
      static void encode(const float128& v, std::vector<char>& s)
      {
         s.insert(s.end(), reinterpret_cast<const char*>(&v),
                  reinterpret_cast<const char*>(&v) + 16);
      }
      static float128 decode(std::span<const char> b) noexcept
      {
         float128 out{};
         std::memcpy(&out, b.data(), 16);
         return out;
      }
      static codec_status validate(std::span<const char> b) noexcept
      {
         return b.size() < 16
                    ? codec_fail("float128: short buffer", 0, "float128")
                    : codec_ok();
      }
      static codec_status validate_strict(std::span<const char> b) noexcept
      {
         return validate(b);
      }
   };

   struct float128_sortable_codec
   {
      static std::size_t packsize(const float128&) noexcept { return 16; }

      static void encode(const float128& v, std::vector<char>& s)
      {
         std::uint64_t hi = v.limb[1];
         std::uint64_t lo = v.limb[0];
         // Canonicalise -0.0 to +0.0 so they compare equal in the
         // sort key (matches the existing double / float behaviour).
         if (hi == 0x8000000000000000ull && lo == 0)
         {
            hi = 0;
         }
         if (hi & 0x8000000000000000ull)
         {
            // Negative: flip every bit.
            hi = ~hi;
            lo = ~lo;
         }
         else
         {
            // Positive: flip just the sign bit so positives sort
            // above negatives.
            hi ^= 0x8000000000000000ull;
         }
         // Big-endian on the wire so memcmp compares MSB first.
         std::uint8_t buf[16];
         for (int i = 0; i < 8; ++i)
            buf[i] = static_cast<std::uint8_t>(hi >> (56 - 8 * i));
         for (int i = 0; i < 8; ++i)
            buf[8 + i] = static_cast<std::uint8_t>(lo >> (56 - 8 * i));
         s.insert(s.end(), reinterpret_cast<const char*>(buf),
                  reinterpret_cast<const char*>(buf) + 16);
      }

      static float128 decode(std::span<const char> b) noexcept
      {
         std::uint64_t hi = 0, lo = 0;
         for (int i = 0; i < 8; ++i)
            hi = (hi << 8) | static_cast<std::uint8_t>(b[i]);
         for (int i = 0; i < 8; ++i)
            lo = (lo << 8) | static_cast<std::uint8_t>(b[8 + i]);
         if (hi & 0x8000000000000000ull)
            hi ^= 0x8000000000000000ull;  // was positive: undo sign flip
         else
         {
            hi = ~hi;                     // was negative: undo bit-not
            lo = ~lo;
         }
         float128 out{};
         out.limb[1] = hi;
         out.limb[0] = lo;
         return out;
      }

      static codec_status validate(std::span<const char> b) noexcept
      {
         return b.size() < 16
                    ? codec_fail("float128: short buffer", 0, "float128")
                    : codec_ok();
      }
      static codec_status validate_strict(std::span<const char> b) noexcept
      {
         return validate(b);
      }
   };

   struct float128_text_codec
   {
      static std::size_t packsize(const float128&) noexcept
      {
         // "0x" + 32 hex chars.
         return 2 + 32;
      }

      static void encode(const float128& v, std::string& s)
      {
         s += "0x";
         for (int shift = 60; shift >= 0; shift -= 4)
            s.push_back(ext_int_detail::hex_digit(
                static_cast<unsigned>((v.limb[1] >> shift) & 0xfu)));
         for (int shift = 60; shift >= 0; shift -= 4)
            s.push_back(ext_int_detail::hex_digit(
                static_cast<unsigned>((v.limb[0] >> shift) & 0xfu)));
      }

      static float128 decode(std::span<const char> in)
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok() || neg || body.size() != 32)
            return float128{};
         std::uint64_t hi = 0, lo = 0;
         for (std::size_t i = 0; i < 16; ++i)
         {
            int d = ext_int_detail::hex_value(body[i]);
            if (d < 0)
               return float128{};
            hi = (hi << 4) | static_cast<std::uint64_t>(d);
         }
         for (std::size_t i = 16; i < 32; ++i)
         {
            int d = ext_int_detail::hex_value(body[i]);
            if (d < 0)
               return float128{};
            lo = (lo << 4) | static_cast<std::uint64_t>(d);
         }
         float128 out{};
         out.limb[1] = hi;
         out.limb[0] = lo;
         return out;
      }

      static codec_status validate(std::span<const char> in) noexcept
      {
         bool                  neg;
         std::span<const char> body;
         auto st = ext_int_detail::strip_framing(in, neg, body);
         if (!st.ok())
            return st;
         if (neg)
            return codec_fail("float128: leading '-' not allowed (encoding "
                              "is signed-magnitude raw bits)", 0, "float128");
         if (body.size() != 32)
            return codec_fail("float128: expected 32 hex chars after 0x", 0,
                              "float128");
         for (char c : body)
            if (ext_int_detail::hex_value(c) < 0)
               return codec_fail("float128: invalid hex digit", 0, "float128");
         return codec_ok();
      }
      static codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };

   // Format codecs detect these as fixed-size primitives: for binary
   // formats the wire is their raw LE bytes. The per-format headers
   // (ssz.hpp / frac.hpp / …) are responsible for emitting the encode
   // and decode cases — this header only declares the types.

}  // namespace psio

PSIO_ADAPTER(psio::uint128, psio::text_category, psio::uint128_text_codec)
PSIO_ADAPTER(psio::int128,  psio::text_category, psio::int128_text_codec)
PSIO_ADAPTER(psio::uint256, psio::text_category, psio::uint256_text_codec)

PSIO_ADAPTER(psio::float128, psio::binary_category,          psio::float128_binary_codec)
PSIO_ADAPTER(psio::float128, psio::sortable_binary_category, psio::float128_sortable_codec)
PSIO_ADAPTER(psio::float128, psio::text_category,            psio::float128_text_codec)
