#pragma once
//
// psio/detail/capnp_holes.hpp — the hole set of Cap'n Proto's struct layout.
//
// The official compiler places a data field in the smallest hole that fits
// (a hole is what is left of a word after a smaller field took its start)
// or in a new word. This is `HoleSet` of capnp's node-translator, shared by
// the compile-time layout of psio/capnp.hpp and the run-time one of
// psio/capnp_layout.hpp.

#include <cstdint>
#include <optional>

namespace psio::detail
{

   struct capnp_hole_set
   {
      // holes[lg]: offset (in units of 2^lg bits) of the one hole of
      // that size; 0 = none (offset 0 is never a hole).
      std::uint32_t holes[6] = {};

      constexpr std::optional<std::uint32_t> try_allocate(std::uint32_t lg)
      {
         if (lg >= 6)
            return std::nullopt;
         if (holes[lg] != 0)
         {
            const auto r = holes[lg];
            holes[lg]    = 0;
            return r;
         }
         if (auto next = try_allocate(lg + 1))
         {
            const auto r = *next * 2;
            holes[lg]    = r + 1;
            return r;
         }
         return std::nullopt;
      }
      constexpr void add_holes_at_end(std::uint32_t lg, std::uint32_t offset,
                                      std::uint32_t limit = 6)
      {
         while (lg < limit)
         {
            holes[lg] = offset;
            ++lg;
            offset = (offset + 1) / 2;
         }
      }
      constexpr bool try_expand(std::uint32_t old_lg, std::uint32_t old_offset,
                                std::uint32_t factor)
      {
         if (factor == 0)
            return true;
         if (old_lg >= 6)
            return false;
         if (holes[old_lg] != old_offset + 1)
            return false;
         if (try_expand(old_lg + 1, old_offset >> 1, factor - 1))
         {
            holes[old_lg] = 0;
            return true;
         }
         return false;
      }
      constexpr std::optional<std::uint32_t> smallest_at_least(std::uint32_t lg) const
      {
         for (std::uint32_t i = lg; i < 6; ++i)
            if (holes[i] != 0)
               return i;
         return std::nullopt;
      }
   };

}  // namespace psio::detail
