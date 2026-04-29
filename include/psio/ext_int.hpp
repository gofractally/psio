#pragma once
//
// psio/ext_int.hpp — 128-bit and 256-bit integer types.
//
// Byte-compatible with psio1::uint128 / psio1::int128 / psio1::uint256 in v1.
// Used for SSZ/Ethereum-consensus workloads (balances, hashes, KZG).
// Wire form is always 16 or 32 raw LE bytes; arithmetic is out of scope
// (users convert to their preferred bignum library via the limb array).

#include <array>
#include <compare>
#include <cstdint>
#include <type_traits>

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

   // Format codecs detect these as fixed-size primitives: for binary
   // formats the wire is their raw LE bytes. The per-format headers
   // (ssz.hpp / frac.hpp / …) are responsible for emitting the encode
   // and decode cases — this header only declares the types.

}  // namespace psio
