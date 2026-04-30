#pragma once
//
// libraries/psio/cpp/include/psio/varint/result.hpp — return types.
//
// Pulled out of the umbrella header so algorithm headers can include
// just this without forming a cycle. Each result describes a single
// decode call; `len` is the number of bytes consumed on success and 0
// on failure (truncated input, oversize, or canonicity violation —
// callers that care which is which should use the `_strict` variants
// or check input length up front).
//

#include <cstdint>

namespace psio::varint {

   struct decode_u32_result
   {
      std::uint32_t value;
      std::uint8_t  len;
      bool          ok;
   };

   struct decode_u64_result
   {
      std::uint64_t value;
      std::uint8_t  len;
      bool          ok;
   };

   struct decode_i32_result
   {
      std::int32_t value;
      std::uint8_t len;
      bool         ok;
   };

   struct decode_i64_result
   {
      std::int64_t value;
      std::uint8_t len;
      bool         ok;
   };

}  // namespace psio::varint
