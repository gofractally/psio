#pragma once
//
// psio/trusted.hpp — the mark of bytes the caller vouches for.

#include <span>

namespace psio
{
   // Marks bytes the caller vouches for: `from_F<T>(trusted{bytes})`
   // decodes them, and `F_buffer<T>{trusted{bytes}}` / `shared_F<T>{
   // trusted{bytes}}` adopt them, without validating first. The
   // construction is explicit, so validation is never skipped by accident.
   struct trusted
   {
      std::span<const char> bytes;

      explicit trusted(std::span<const char> b) noexcept : bytes(b) {}
   };
}  // namespace psio
