#pragma once
//
// psio/storage.hpp — storage-policy tag for buffer / view / mutable_view.
//
// Orthogonal to (T, Fmt). Read ops work on every variant; write ops
// require a backing with exactly one writer (owning, mut_borrow).
//
// A read view (psio::view) has two kinds: const_borrow, the SAFE view, which
// compares every offset it reads before using it, and trusted_borrow, the
// TRUSTED view of bytes the caller vouches for (view.hpp "Safe and trusted
// views"), which does not.

namespace psio {

   enum class storage
   {
      owning,       // std::vector<char>-backed; buffer owns its bytes
      mut_borrow,   // std::span<char> — mutable borrow of foreign bytes
      const_borrow, // std::span<const char> — immutable borrow
      shared,       // reference-counted immutable bytes; copies share them
      trusted_borrow, // std::span<const char> — immutable borrow of bytes that
                      // are known to be a valid encoding (views only)
   };

}  // namespace psio
