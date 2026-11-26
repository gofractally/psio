#pragma once
// The VALIDATED write of a value in any format, for tests written over a
// format parameter: psio::encode_checked(F{}, v, out), raising as
// psio::to_F_checked(v) does. (psio::encode(F{}, v) is the fast write: it
// does not hold a value against what its type declares.)
#include <psio/error.hpp>
#include <psio/helpers.hpp>

#include <span>
#include <string>
#include <vector>

namespace psio_test
{
   template <typename F, typename T, typename Sink>
   void encode_validated(F format, const T& value, Sink& sink)
   {
      if (auto status = ::psio::encode_checked(format, value, sink); !status.ok())
         ::psio::codec_abort(status.error());
   }
   template <typename F, typename T>
   auto encode_validated(F format, const T& value)
   {
      ::psio::detail::helpers::checked_buffer_t<F> out;
      encode_validated(format, value, out);
      return out;
   }

   // The VALIDATING read of `bytes` as a T in format F: what
   // psio::from_F<T>(bytes) does. (psio::decode<T>(F{}, bytes) is the trusted
   // read: it does not hold the value against what T declares.)
   template <typename T, typename F>
   T decode_validated(F format, std::span<const char> bytes)
   {
      return ::psio::detail::helpers::decode_checked<T>(format, bytes);
   }
}  // namespace psio_test
