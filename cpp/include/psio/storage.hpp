#pragma once
//
// psio/storage.hpp — storage-policy tag for buffer / view / mutable_view.
//
// Orthogonal to (T, Fmt). Read ops work on every variant; write ops
// require a backing with exactly one writer (owning, mut_borrow).

namespace psio {

   enum class storage
   {
      owning,       // std::vector<char>-backed; buffer owns its bytes
      mut_borrow,   // std::span<char> — mutable borrow of foreign bytes
      const_borrow, // std::span<const char> — immutable borrow
      shared,       // reference-counted immutable bytes; copies share them
   };

}  // namespace psio
