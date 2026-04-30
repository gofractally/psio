#pragma once
//
// psio/bytes_view.hpp — byte-span aliases.
//
// Direct port of psio/bytes_view.hpp.  Used as the canonical span
// type at API boundaries throughout the stack (pfs, psitri, etc.).

#include <cstdint>
#include <span>

namespace psio {
   using bytes_view         = std::span<const std::uint8_t>;
   using mutable_bytes_view = std::span<std::uint8_t>;
}  // namespace psio
