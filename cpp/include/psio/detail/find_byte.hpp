#pragma once
//
// psio/detail/find_byte.hpp — SWAR 8-byte-at-a-time scalar byte search.
//
// Vendored from psitri's `ucc::find_byte`
// (https://github.com/gofractally/arbtrie  libraries/ucc/include/ucc/lower_bound.hpp).
// Same algorithm — broadcast the target into a u64, XOR against an
// 8-byte-aligned chunk, drop matches with the (x − 0x01..) & ~x trick,
// mask high bits to isolate the matching lane, ctz for the index. Tail
// of <8 bytes is handled with a u32 path then a scalar loop. Faster
// than std::memchr on small buffers (no function-call overhead, no
// dispatch); same wallclock as memchr beyond ~64 bytes.
//
// We vendor only this one function rather than depend on ucc as a
// whole — it's the only ucc symbol psio uses.

#include <cstdint>
#include <cstddef>

namespace psio::detail {

   /// First-occurrence byte search.  Returns the index of the first
   /// byte equal to `value` in [arr, arr+size), or `size` if not
   /// found.  Reads up to 7 bytes past the matching position via the
   /// 8-byte chunk path; callers must guarantee `arr` is followed by
   /// at least `(8 - size%8) % 8` readable bytes when `size > 0`, OR
   /// pass a buffer whose size is a multiple of 8.  Both call sites
   /// in pjson_view.hpp satisfy the latter (pjson lays out the hash
   /// table in 8-byte aligned chunks).
   inline int find_byte(const std::uint8_t* arr,
                         std::size_t         size,
                         std::uint8_t        value) noexcept
   {
      const std::uint64_t target =
         value * 0x0101010101010101ULL;             // broadcast
      const std::uint8_t* end      = arr + size;
      const std::uint8_t* last_pos = end - 8;
      const std::uint8_t* p        = arr;

      while (p <= last_pos)
      {
         const std::uint64_t data            = *(const std::uint64_t*)p;
         const std::uint64_t data_xor_target = data ^ target;
         std::uint64_t       mask =
            (data_xor_target - 0x0101010101010101ULL) & ~data_xor_target;
         mask &= 0x8080808080808080ULL;

         if (mask)
         {
            const std::size_t offset = static_cast<std::size_t>(p - arr);
            return static_cast<int>(offset + (__builtin_ctzll(mask) >> 3));
         }
         p += 8;
      }

      const auto remaining = end - p;
      if (remaining >= 4)
      {
         const std::uint32_t data_xor_target =
            *(const std::uint32_t*)p ^ static_cast<std::uint32_t>(target);
         std::uint32_t mask =
            (data_xor_target - 0x01010101u) & ~data_xor_target;
         mask &= 0x80808080u;

         if (mask)
         {
            const std::size_t offset = static_cast<std::size_t>(p - arr);
            return static_cast<int>(offset + (__builtin_ctzll(mask) >> 3));
         }
         p += 4;
      }
      while (p < end)
      {
         if (*p == value)
            return static_cast<int>(p - arr);
         ++p;
      }
      return static_cast<int>(size);
   }

}  // namespace psio::detail
