// Format-neutral conformance harness (§4.4 / §8 #4).
//
// One fixture, one macro, every compatible format covered. Adding a new
// format to `PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT` in conformance.hpp
// automatically extends every TEST_CASE here.

#include <psio/conformance.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace conformance {

   struct Point
   {
      std::int32_t x;
      std::int32_t y;
   };
   PSIO_REFLECT(Point, x, y)

   struct Blob
   {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO_REFLECT(Blob, version, payload, note)

}  // namespace conformance

// Round-trip a fixed record across every symmetric binary format.
#define CONFORMANCE_POINT_ROUND_TRIP(FmtName, FmtValue)              \
   TEST_CASE("conformance [" #FmtName "]: round-trip Point",         \
             "[conformance][" #FmtName "]")                          \
   {                                                                 \
      conformance::Point in{3, -7};                                  \
      auto bytes = psio::encode(FmtValue, in);                      \
      auto out   = psio::decode<conformance::Point>(                \
         FmtValue, std::span<const char>{bytes});                    \
      REQUIRE(out.x == 3);                                           \
      REQUIRE(out.y == -7);                                          \
   }
PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(CONFORMANCE_POINT_ROUND_TRIP)
#undef CONFORMANCE_POINT_ROUND_TRIP

// Round-trip a variable record across every symmetric binary format.
#define CONFORMANCE_BLOB_ROUND_TRIP(FmtName, FmtValue)               \
   TEST_CASE("conformance [" #FmtName "]: round-trip Blob",          \
             "[conformance][" #FmtName "]")                          \
   {                                                                 \
      conformance::Blob in{42, {1, 2, 3}, "hi"};                     \
      auto bytes = psio::encode(FmtValue, in);                      \
      auto out   = psio::decode<conformance::Blob>(                 \
         FmtValue, std::span<const char>{bytes});                    \
      REQUIRE(out.version == 42);                                    \
      REQUIRE(out.payload == std::vector<std::uint32_t>{1, 2, 3});   \
      REQUIRE(out.note == "hi");                                     \
   }
PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(CONFORMANCE_BLOB_ROUND_TRIP)
#undef CONFORMANCE_BLOB_ROUND_TRIP

// Empty-container edge case: Blob with empty payload + empty note.
#define CONFORMANCE_EMPTY_BLOB(FmtName, FmtValue)                    \
   TEST_CASE("conformance [" #FmtName "]: empty Blob",               \
             "[conformance][" #FmtName "]")                          \
   {                                                                 \
      conformance::Blob in{0, {}, ""};                               \
      auto bytes = psio::encode(FmtValue, in);                      \
      auto out   = psio::decode<conformance::Blob>(                 \
         FmtValue, std::span<const char>{bytes});                    \
      REQUIRE(out.version == 0);                                     \
      REQUIRE(out.payload.empty());                                  \
      REQUIRE(out.note == "");                                       \
   }
PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(CONFORMANCE_EMPTY_BLOB)
#undef CONFORMANCE_EMPTY_BLOB

// std::variant — one code path, 8 formats.
#define CONFORMANCE_VARIANT_ROUND_TRIP(FmtName, FmtValue)                   \
   TEST_CASE("conformance [" #FmtName "]: round-trip variant",              \
             "[conformance][" #FmtName "][variant]")                        \
   {                                                                        \
      using V = std::variant<std::uint32_t, std::string>;                   \
      V    a = std::uint32_t{0xDEADBEEF};                                   \
      auto ab   = psio::encode(FmtValue, a);                               \
      auto aout = psio::decode<V>(FmtValue, std::span<const char>{ab});    \
      REQUIRE(aout.index() == 0);                                           \
      REQUIRE(std::get<0>(aout) == 0xDEADBEEF);                             \
      V    b = std::string("hi");                                           \
      auto bb   = psio::encode(FmtValue, b);                               \
      auto bout = psio::decode<V>(FmtValue, std::span<const char>{bb});    \
      REQUIRE(bout.index() == 1);                                           \
      REQUIRE(std::get<1>(bout) == "hi");                                   \
   }
PSIO_FOR_EACH_RECORD_BINARY_FMT(CONFORMANCE_VARIANT_ROUND_TRIP)
#undef CONFORMANCE_VARIANT_ROUND_TRIP

// Cross-format conversion (§8 criterion #5): decode via one format,
// re-encode via another, assert the value round-trips. The outer loop
// picks From; the inner "to-all-formats" assertion runs every target
// as a Catch2 SECTION.
namespace {
   template <typename From, typename To>
   void cross_format_blob(From from, To to)
   {
      conformance::Blob in{7, {10, 20, 30}, "xfer"};
      auto from_bytes = psio::encode(from, in);
      auto mid = psio::decode<conformance::Blob>(
         from, std::span<const char>{from_bytes});
      auto to_bytes = psio::encode(to, mid);
      auto out = psio::decode<conformance::Blob>(
         to, std::span<const char>{to_bytes});
      REQUIRE(out.version == 7);
      REQUIRE(out.payload == std::vector<std::uint32_t>{10, 20, 30});
      REQUIRE(out.note == "xfer");
   }

   template <typename From>
   void cross_to_all(From from)
   {
      cross_format_blob(from, ::psio::ssz{});
      cross_format_blob(from, ::psio::pssz{});
      cross_format_blob(from, ::psio::frac32{});
      cross_format_blob(from, ::psio::bin{});
      cross_format_blob(from, ::psio::borsh{});
      cross_format_blob(from, ::psio::bincode{});
      cross_format_blob(from, ::psio::avro{});
      cross_format_blob(from, ::psio::key{});
      cross_format_blob(from, ::psio::capnp{});
      cross_format_blob(from, ::psio::wit{});
   }
}  // namespace

#define CROSS_FROM(FmtName, FmtValue)                                \
   TEST_CASE("conformance cross-format from " #FmtName " → all",    \
             "[conformance][cross][" #FmtName "]")                   \
   {                                                                 \
      cross_to_all(FmtValue);                                        \
   }
PSIO_FOR_EACH_SYMMETRIC_BINARY_FMT(CROSS_FROM)
#undef CROSS_FROM

// bitvector<N> — every symmetric binary format supports it now.
#define CONFORMANCE_BITVECTOR(FmtName, FmtValue)                     \
   TEST_CASE("conformance [" #FmtName "]: bitvector<24> round-trip", \
             "[conformance][" #FmtName "][bitvector]")               \
   {                                                                 \
      psio::bitvector<24> in;                                       \
      for (std::size_t i : {0, 7, 15, 23})                           \
         in.set(i, true);                                            \
      auto bv  = psio::encode(FmtValue, in);                        \
      auto out = psio::decode<psio::bitvector<24>>(                \
         FmtValue, std::span<const char>{bv});                       \
      for (std::size_t i = 0; i < 24; ++i)                           \
         REQUIRE(out.test(i) == in.test(i));                         \
   }
PSIO_FOR_EACH_RECORD_BINARY_FMT(CONFORMANCE_BITVECTOR)
#undef CONFORMANCE_BITVECTOR

// uint256 — all 8 formats.
#define CONFORMANCE_UINT256(FmtName, FmtValue)                       \
   TEST_CASE("conformance [" #FmtName "]: uint256 round-trip",       \
             "[conformance][" #FmtName "][extint]")                  \
   {                                                                 \
      psio::uint256 in;                                             \
      in.limb[0] = 0xDEADBEEFCAFEBABE;                               \
      in.limb[1] = 0x1234567890ABCDEF;                               \
      in.limb[2] = 0x0F0F0F0F0F0F0F0F;                               \
      in.limb[3] = 0xFFFFFFFFFFFFFFFF;                               \
      auto bv = psio::encode(FmtValue, in);                         \
      auto out = psio::decode<psio::uint256>(                      \
         FmtValue, std::span<const char>{bv});                       \
      REQUIRE(out == in);                                            \
   }
PSIO_FOR_EACH_RECORD_BINARY_FMT(CONFORMANCE_UINT256)
#undef CONFORMANCE_UINT256
