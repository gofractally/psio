// Regression guard for two bug classes that surfaced in v1 psio and
// could have reappeared in psio:
//
//   Bug 1 — nested-reflected memcpy fast path that assumed C++ sizeof
//           matches the wire fixed_size. Broken when the struct has
//           internal padding (e.g. `struct { u8 a; u64 b; }` is 16
//           bytes in C++ but 9 bytes on the fracpack wire). psio's
//           codecs walk reflected records field-by-field — this test
//           pins that behavior.
//
//   Bug 2 — view/buffer methods named `data()` / `size()` shadowing
//           reflected field accessors. psio uses `_psio_data()` and
//           exposes storage ops as free functions; this test pins the
//           rule by defining a reflected type with a field literally
//           named `data` and encoding/decoding through every format.
//
// Both classes were discovered by a consumer hitting runtime-wrong
// bytes in v1. Keeping these tests here means psio can't regress
// into the same shape.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace reg {

   // Padded inner — sizeof(C++) = 16 due to alignment, wire fixed_size
   // on a packed codec = 9. A memcpy of 16 bytes would mis-read wire
   // bytes; codec must walk field-by-field.
   struct PaddedInner
   {
      std::uint8_t  a;   // 1 byte
      std::uint64_t b;   // 8 bytes @ offset 8 in C++, @ offset 1 on wire
   };
   PSIO_REFLECT(PaddedInner, a, b)

   struct WithInner
   {
      PaddedInner             inner;
      std::uint32_t           id;
   };
   PSIO_REFLECT(WithInner, inner, id)

   // Fields named `data` / `size` / `bytes` — the exact names v1's
   // view methods collided with.
   struct Segment
   {
      std::uint64_t             id;
      std::vector<std::uint8_t> data;
      std::uint32_t             size;
   };
   PSIO_REFLECT(Segment, id, data, size)

}  // namespace reg

#define PADDING_CASE(FmtName, FmtValue)                                \
   TEST_CASE("padding: PaddedInner nested roundtrip [" #FmtName "]",   \
             "[padding][" #FmtName "]")                                \
   {                                                                   \
      reg::WithInner in{{0xAB, 0xDEADBEEFCAFEBABEULL}, 42};            \
      auto bv = psio::encode(FmtValue, in);                           \
      auto out = psio::decode<reg::WithInner>(                        \
         FmtValue, std::span<const char>{bv});                         \
      REQUIRE(out.inner.a == 0xAB);                                    \
      REQUIRE(out.inner.b == 0xDEADBEEFCAFEBABEULL);                   \
      REQUIRE(out.id      == 42);                                      \
   }
PADDING_CASE(ssz,     ::psio::ssz{})
PADDING_CASE(pssz32,  ::psio::pssz32{})
PADDING_CASE(frac32,  ::psio::frac32{})
PADDING_CASE(bin,     ::psio::bin{})
PADDING_CASE(borsh,   ::psio::borsh{})
PADDING_CASE(bincode, ::psio::bincode{})
PADDING_CASE(avro,    ::psio::avro{})
#undef PADDING_CASE

#define FIELD_NAME_CASE(FmtName, FmtValue)                             \
   TEST_CASE("field names: Segment with `data`/`size` [" #FmtName "]", \
             "[fieldname][" #FmtName "]")                              \
   {                                                                   \
      reg::Segment in{99, {0x01, 0x02, 0x03, 0x04}, 12345};            \
      auto bv = psio::encode(FmtValue, in);                           \
      auto out = psio::decode<reg::Segment>(                          \
         FmtValue, std::span<const char>{bv});                         \
      REQUIRE(out.id   == 99);                                         \
      REQUIRE(out.data == std::vector<std::uint8_t>{1, 2, 3, 4});      \
      REQUIRE(out.size == 12345);                                      \
   }
FIELD_NAME_CASE(ssz,     ::psio::ssz{})
FIELD_NAME_CASE(pssz32,  ::psio::pssz32{})
FIELD_NAME_CASE(frac32,  ::psio::frac32{})
FIELD_NAME_CASE(bin,     ::psio::bin{})
FIELD_NAME_CASE(borsh,   ::psio::borsh{})
FIELD_NAME_CASE(bincode, ::psio::bincode{})
FIELD_NAME_CASE(avro,    ::psio::avro{})
FIELD_NAME_CASE(json,    ::psio::json{})
#undef FIELD_NAME_CASE
