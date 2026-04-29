#pragma once
//
// psio/storage.hpp — storage-policy tag for buffer / view / mutable_view.
//
// Orthogonal to (T, Fmt). Read ops work on all three variants; write
// ops require non-const backings.

namespace psio {

   enum class storage
   {
      owning,       // std::vector<char>-backed; buffer owns its bytes
      mut_borrow,   // std::span<char> — mutable borrow of foreign bytes
      const_borrow, // std::span<const char> — immutable borrow
   };

}  // namespace psio
