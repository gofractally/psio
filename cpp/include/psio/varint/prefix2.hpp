#pragma once
//
// libraries/psio/cpp/include/psio/varint/prefix2.hpp — 2-bit prefix.
//
// Two flavors of the same wire layout:
//
//   prefix2::be   — RFC 9000 / QUIC canonical: 2 high bits of byte 0
//                   = log2(length), payload big-endian in the
//                   remaining bits.
//   prefix2::le   — identical layout but payloads stored
//                   little-endian, matching the rest of the v3 wire
//                   formats. Provided for direct head-to-head
//                   benchmarking against `be` on this codebase's
//                   target hosts.
//
//   code  total bytes  payload bits   value range
//   ----  -----------  ------------   -------------
//    00       1            6          [0,  2^6)
//    01       2            14         [2^6, 2^14)
//    10       4            30         [2^14, 2^30)
//    11       8            62         [2^30, 2^62)
//
// Note the 62-bit ceiling on the largest variant — QUIC's choice, not
// ours; values >= 2^62 cannot be represented and the encoder will
// return 0 (caller must check).  For full u64 coverage use
// `varint::prefix3`.
//

#include <psio/varint/detail/cpu.hpp>
#include <psio/varint/result.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace psio::varint::prefix2 {

   inline constexpr std::size_t max_bytes_u64 = 8;
   inline constexpr std::uint64_t max_value =
      (static_cast<std::uint64_t>(1) << 62) - 1;

   namespace detail {

      // Map a value to its prefix code (00, 01, 10, 11) and to the
      // total wire byte count (1, 2, 4, 8).
      inline constexpr unsigned code_for(std::uint64_t v) noexcept
      {
         if (v < (1ULL << 6))  return 0;
         if (v < (1ULL << 14)) return 1;
         if (v < (1ULL << 30)) return 2;
         return 3;
      }

      inline constexpr std::size_t bytes_for_code(unsigned code) noexcept
      {
         return static_cast<std::size_t>(1) << code;
      }

   }  // namespace detail

   inline constexpr std::size_t size_u64(std::uint64_t v) noexcept
   {
      return detail::bytes_for_code(detail::code_for(v));
   }

   // Returns 0 if `v > max_value` (caller responsibility to check).
   // Otherwise emits the canonical encoding.

   // ────────────────────────────────────────────────────────────────
   // be — canonical QUIC.
   // ────────────────────────────────────────────────────────────────
   namespace be {

      namespace scalar {

         inline std::size_t encode_u64(std::uint8_t* buf,
                                       std::uint64_t v) noexcept
         {
            if (v > max_value) return 0;
            const unsigned code  = detail::code_for(v);
            const std::size_t n  = detail::bytes_for_code(code);
            // Pack code into top 2 bits of byte 0; remaining bits hold
            // the high 8*n - 2 bits of payload, big-endian.
            std::uint64_t with_prefix =
               v | (static_cast<std::uint64_t>(code) << (8 * n - 2));
            for (std::size_t i = 0; i < n; ++i)
               buf[i] = static_cast<std::uint8_t>(
                  with_prefix >> (8 * (n - 1 - i)));
            return n;
         }

         inline decode_u64_result decode_u64(const std::uint8_t* p,
                                             std::size_t avail) noexcept
         {
            if (avail == 0) return {0, 0, false};
            const unsigned    code = static_cast<unsigned>(p[0]) >> 6;
            const std::size_t n    = detail::bytes_for_code(code);
            if (avail < n) return {0, 0, false};
            std::uint64_t v = static_cast<std::uint64_t>(p[0] & 0x3F);
            for (std::size_t i = 1; i < n; ++i)
               v = (v << 8) | p[i];
            return {v, static_cast<std::uint8_t>(n), true};
         }

      }  // namespace scalar

      namespace fast {

         // Single-load fast path: read 8 bytes, byteswap once, extract
         // code from the top 2 bits, mask off the prefix, shift the
         // payload down by (8 - n) bytes. Falls back to scalar when
         // fewer than 8 readable bytes remain (the safe-tail path).
         inline decode_u64_result decode_u64(const std::uint8_t* p,
                                             std::size_t avail) noexcept
         {
            if (avail < 8) return scalar::decode_u64(p, avail);
            std::uint64_t x;
            std::memcpy(&x, p, 8);
            x = std::byteswap(x);  // bytes now laid out as a BE u64.
            const unsigned    code = static_cast<unsigned>(x >> 62);
            const std::size_t n    = detail::bytes_for_code(code);
            const std::uint64_t payload_mask =
               (static_cast<std::uint64_t>(1) << 62) - 1;
            const std::uint64_t v = (x & payload_mask) >> (8 * (8 - n));
            return {v, static_cast<std::uint8_t>(n), true};
         }

         using scalar::encode_u64;

      }  // namespace fast

      using fast::decode_u64;
      using fast::encode_u64;
      using ::psio::varint::prefix2::size_u64;

   }  // namespace be

   // ────────────────────────────────────────────────────────────────
   // le — same prefix layout, little-endian payload bytes.
   //
   // Encoding for a value v with code c (length n = 1<<c):
   //   first byte's low 2 bits = c
   //   remaining (8n - 2) bits = payload, byte 0 carries the LOW 6
   //   bits, byte 1 the next 8, etc.
   // ────────────────────────────────────────────────────────────────
   namespace le {

      namespace scalar {

         inline std::size_t encode_u64(std::uint8_t* buf,
                                       std::uint64_t v) noexcept
         {
            if (v > max_value) return 0;
            const unsigned    code = detail::code_for(v);
            const std::size_t n    = detail::bytes_for_code(code);
            const std::uint64_t with_prefix =
               static_cast<std::uint64_t>(code) | (v << 2);
            std::memcpy(buf, &with_prefix, n);
            return n;
         }

         inline decode_u64_result decode_u64(const std::uint8_t* p,
                                             std::size_t avail) noexcept
         {
            if (avail == 0) return {0, 0, false};
            const unsigned    code = static_cast<unsigned>(p[0]) & 0x3;
            const std::size_t n    = detail::bytes_for_code(code);
            if (avail < n) return {0, 0, false};
            std::uint64_t x = 0;
            std::memcpy(&x, p, n);
            return {x >> 2, static_cast<std::uint8_t>(n), true};
         }

      }  // namespace scalar

      namespace fast {

         inline decode_u64_result decode_u64(const std::uint8_t* p,
                                             std::size_t avail) noexcept
         {
            if (avail < 8) return scalar::decode_u64(p, avail);
            std::uint64_t x;
            std::memcpy(&x, p, 8);
            const unsigned    code = static_cast<unsigned>(x) & 0x3;
            const std::size_t n    = detail::bytes_for_code(code);
            // Mask off the bytes beyond `n`. For n=8 this is a no-op;
            // for shorter forms the high bits we accidentally loaded
            // get cleared.
            const std::uint64_t mask =
               (n == 8) ? ~static_cast<std::uint64_t>(0)
                        : ((static_cast<std::uint64_t>(1) << (8 * n)) - 1);
            const std::uint64_t v = (x & mask) >> 2;
            return {v, static_cast<std::uint8_t>(n), true};
         }

         using scalar::encode_u64;

      }  // namespace fast

      using fast::decode_u64;
      using fast::encode_u64;
      using ::psio::varint::prefix2::size_u64;

   }  // namespace le

}  // namespace psio::varint::prefix2
