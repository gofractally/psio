#pragma once
//
// libraries/psio/cpp/include/psio/varint/prefix3.hpp — 3-bit prefix.
//
// Layout (top 3 bits of byte 0 = code, remaining bits = payload):
//
//   code  total bytes  payload bits     value range
//   ----  -----------  ------------     ------------------
//   000        1            5           [0,         2^5)
//   001        2            13          [2^5,       2^13)
//   010        3            21          [2^13,      2^21)
//   011        4            29          [2^21,      2^29)
//   100        5            37          [2^29,      2^37)
//   101        6            45          [2^37,      2^45)
//   110        7            53          [2^45,      2^53)
//   111        9            64 (escape) [2^53,      2^64)
//
// Codes 0–6: payload = (byte0 & 0x1F) | (next bytes interpreted as
// little-endian unsigned integer << 5).  Length = code + 1.
//
// Code 7: byte 0's low 5 bits are reserved-zero (encoder writes 0;
// strict decoder rejects non-zero).  The next 8 bytes are the raw
// little-endian u64 payload — covers the full 64-bit range.  Total 9
// bytes.  We skip length-8 deliberately: 5 + 7×8 = 61 bits would not
// reach a full u64, and a 9-byte escape gives the cleanest single-step
// to full coverage without growing the prefix to 4 bits.
//
// Canonicity: the smallest representable encoding is the one each
// value MUST use.  `decode_u64_strict` enforces this; `decode_u64`
// accepts any wire that fits the format.  All encoders produce
// canonical output.
//

#include <psio/varint/detail/cpu.hpp>
#include <psio/varint/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace psio::varint::prefix3 {

   inline constexpr std::size_t max_bytes_u64 = 9;

   namespace scalar {

      // Number of bytes the canonical encoding will consume. Branchless
      // table lookup keyed on the bit position of the highest set bit.
      inline constexpr std::size_t size_u64(std::uint64_t v) noexcept
      {
         // Bits  →  bytes:
         //  [0,   5]  → 1
         //  [6,  13]  → 2
         //  [14, 21]  → 3
         //  [22, 29]  → 4
         //  [30, 37]  → 5
         //  [38, 45]  → 6
         //  [46, 53]  → 7
         //  [54, 64]  → 9
         if (v < (1ULL << 5))  return 1;
         if (v < (1ULL << 13)) return 2;
         if (v < (1ULL << 21)) return 3;
         if (v < (1ULL << 29)) return 4;
         if (v < (1ULL << 37)) return 5;
         if (v < (1ULL << 45)) return 6;
         if (v < (1ULL << 53)) return 7;
         return 9;
      }

      inline std::size_t encode_u64(std::uint8_t* buf, std::uint64_t v) noexcept
      {
         if (v < (1ULL << 5))
         {
            buf[0] = static_cast<std::uint8_t>(v);
            return 1;
         }
         if (v < (1ULL << 53))
         {
            // Codes 1..6: top 3 bits of byte 0 = code, low 5 = v[0..4].
            // Trailing (code) bytes are LE payload starting at bit 5.
            unsigned code = 1;
            if (v >= (1ULL << 13)) code = 2;
            if (v >= (1ULL << 21)) code = 3;
            if (v >= (1ULL << 29)) code = 4;
            if (v >= (1ULL << 37)) code = 5;
            if (v >= (1ULL << 45)) code = 6;

            buf[0] = static_cast<std::uint8_t>(
               (code << 5) | static_cast<std::uint8_t>(v & 0x1F));
            const std::uint64_t rest = v >> 5;
            std::memcpy(buf + 1, &rest, code);  // unaligned LE store
            return 1u + code;
         }
         // Escape: code 7, 9 bytes total. Low 5 bits of byte 0 zero.
         buf[0] = 0xE0;
         std::memcpy(buf + 1, &v, 8);
         return 9;
      }

      inline decode_u64_result decode_u64(const std::uint8_t* p,
                                          std::size_t          avail) noexcept
      {
         if (avail == 0) return {0, 0, false};
         const std::uint8_t first = p[0];
         const unsigned     code  = static_cast<unsigned>(first) >> 5;
         if (code == 0) return {static_cast<std::uint64_t>(first & 0x1F), 1,
                                true};
         if (code < 7)
         {
            const std::size_t len = static_cast<std::size_t>(code) + 1;
            if (avail < len) return {0, 0, false};
            std::uint64_t rest = 0;
            std::memcpy(&rest, p + 1, len - 1);
            const std::uint64_t v =
               static_cast<std::uint64_t>(first & 0x1F) | (rest << 5);
            return {v, static_cast<std::uint8_t>(len), true};
         }
         // code == 7
         if (avail < 9) return {0, 0, false};
         std::uint64_t v = 0;
         std::memcpy(&v, p + 1, 8);
         return {v, 9, true};
      }

      // Strict decode — additionally rejects:
      //   • non-canonical encoding (a value that would fit in a
      //     shorter form),
      //   • code 7 with non-zero reserved bits in byte 0.
      inline decode_u64_result
         decode_u64_strict(const std::uint8_t* p, std::size_t avail) noexcept
      {
         const auto r = decode_u64(p, avail);
         if (!r.ok) return r;
         if (r.len != size_u64(r.value)) return {0, 0, false};
         if (r.len == 9 && (p[0] & 0x1F) != 0) return {0, 0, false};
         return r;
      }

   }  // namespace scalar

   namespace fast {
      // The scalar form is already a single load + mask + shift; SIMD
      // wouldn't help on a per-call basis. Aliased; revisit if a batch
      // decode API gets added.
      using scalar::decode_u64;
      using scalar::decode_u64_strict;
      using scalar::encode_u64;
      using scalar::size_u64;
   }  // namespace fast

   using fast::decode_u64;
   using fast::decode_u64_strict;
   using fast::encode_u64;
   using fast::size_u64;

}  // namespace psio::varint::prefix3
