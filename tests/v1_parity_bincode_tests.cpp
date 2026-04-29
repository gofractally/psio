// v1 ↔ psio bincode byte-parity tests.
//
// Bincode (default) wire (MVP scope):
//   primitives raw LE, bool = 1 byte, string = u64 len + bytes,
//   vec = u64 len + contents, array = contents only,
//   optional = u8 tag + value, record = fields concatenated.

#include <psio1/bitset.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/reflect.hpp>
#include <psio1/to_bincode.hpp>
#include <psio/bincode.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("bincode parity: primitives", "[bincode][parity][primitive]")
{
   REQUIRE(psio1::convert_to_bincode(std::uint32_t{0xDEADBEEF}) ==
           psio::encode(psio::bincode{}, std::uint32_t{0xDEADBEEF}));
   REQUIRE(psio1::convert_to_bincode(std::uint64_t{0x1122334455667788ULL}) ==
           psio::encode(psio::bincode{},
                         std::uint64_t{0x1122334455667788ULL}));
   REQUIRE(psio1::convert_to_bincode(true) ==
           psio::encode(psio::bincode{}, true));
   REQUIRE(psio1::convert_to_bincode(false) ==
           psio::encode(psio::bincode{}, false));
}

TEST_CASE("bincode parity: std::array<u32, N>", "[bincode][parity][array]")
{
   std::array<std::uint32_t, 4> a{1, 2, 3, 4};
   REQUIRE(psio1::convert_to_bincode(a) == psio::encode(psio::bincode{}, a));
}

TEST_CASE("bincode parity: std::vector<u32>", "[bincode][parity][vector]")
{
   std::vector<std::uint32_t> v{10, 20, 30, 40};
   REQUIRE(psio1::convert_to_bincode(v) == psio::encode(psio::bincode{}, v));
   std::vector<std::uint32_t> empty;
   REQUIRE(psio1::convert_to_bincode(empty) ==
           psio::encode(psio::bincode{}, empty));
}

TEST_CASE("bincode parity: std::string", "[bincode][parity][string]")
{
   REQUIRE(psio1::convert_to_bincode(std::string("hello world")) ==
           psio::encode(psio::bincode{}, std::string("hello world")));
   REQUIRE(psio1::convert_to_bincode(std::string{}) ==
           psio::encode(psio::bincode{}, std::string{}));
}

TEST_CASE("bincode parity: std::optional", "[bincode][parity][optional]")
{
   std::optional<std::uint32_t> n;
   std::optional<std::uint32_t> s = 0xDEADBEEF;
   REQUIRE(psio1::convert_to_bincode(n) == psio::encode(psio::bincode{}, n));
   REQUIRE(psio1::convert_to_bincode(s) == psio::encode(psio::bincode{}, s));
}

namespace v1_bincode {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO1_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO1_REFLECT(Blob, version, payload, note)
}
namespace v3_bincode {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO_REFLECT(Blob, version, payload, note)
}

TEST_CASE("bincode parity: fixed record", "[bincode][parity][record]")
{
   v1_bincode::Point a{3, -7};
   v3_bincode::Point b{3, -7};
   REQUIRE(psio1::convert_to_bincode(a) == psio::encode(psio::bincode{}, b));
}

TEST_CASE("bincode parity: variable record", "[bincode][parity][record]")
{
   v1_bincode::Blob a{7, {100, 200, 300}, "label"};
   v3_bincode::Blob b{7, {100, 200, 300}, "label"};
   REQUIRE(psio1::convert_to_bincode(a) == psio::encode(psio::bincode{}, b));
}

TEST_CASE("bincode parity: v1 encode → v3 decode (Blob)",
          "[bincode][parity][record][round-trip]")
{
   v1_bincode::Blob a{42, {1, 2, 3}, "xx"};
   auto             av = psio1::convert_to_bincode(a);
   auto             back =
      psio::decode<v3_bincode::Blob>(psio::bincode{},
                                       std::span<const char>{av});
   REQUIRE(back.version == 42);
   REQUIRE(back.payload == std::vector<std::uint32_t>{1, 2, 3});
   REQUIRE(back.note == "xx");
}

TEST_CASE("bincode parity: v3 encode → v1 decode (Blob)",
          "[bincode][parity][record][round-trip]")
{
   v3_bincode::Blob b{99, {7, 8}, "yo"};
   auto             bv = psio::encode(psio::bincode{}, b);
   auto back = psio1::convert_from_bincode<v1_bincode::Blob>(bv);
   REQUIRE(back.version == 99);
   REQUIRE(back.payload == std::vector<std::uint32_t>{7, 8});
   REQUIRE(back.note == "yo");
}

TEST_CASE("bincode parity: std::variant", "[bincode][parity][variant]")
{
   using V = std::variant<std::uint32_t, std::string>;
   REQUIRE(psio1::convert_to_bincode(V{std::uint32_t{0xDEADBEEF}}) ==
           psio::encode(psio::bincode{}, V{std::uint32_t{0xDEADBEEF}}));
   REQUIRE(psio1::convert_to_bincode(V{std::string("abc")}) ==
           psio::encode(psio::bincode{}, V{std::string("abc")}));
}

TEST_CASE("bincode round-trip: std::variant",
          "[bincode][parity][variant][round-trip]")
{
   using V = std::variant<std::uint32_t, std::string>;
   V      in = std::string("zzz");
   auto   bv = psio::encode(psio::bincode{}, in);
   auto   out =
      psio::decode<V>(psio::bincode{}, std::span<const char>{bv});
   REQUIRE(out.index() == 1);
   REQUIRE(std::get<1>(out) == "zzz");
}

// ── bitvector / bitlist parity ────────────────────────────────────────

TEST_CASE("bincode parity: bitvector<24>", "[bincode][parity][bitvector]")
{
   psio1::bitvector<24>  v1v;
   psio::bitvector<24> v3v;
   for (std::size_t i : {0, 8, 23})
   {
      v1v.set(i, true);
      v3v.set(i, true);
   }
   REQUIRE(psio1::convert_to_bincode(v1v) ==
           psio::encode(psio::bincode{}, v3v));
}

TEST_CASE("bincode parity: bitlist<256>", "[bincode][parity][bitlist]")
{
   psio1::bitlist<256>  v1l;
   psio::bitlist<256> v3l;
   for (int i = 0; i < 20; ++i)
   {
      bool on = (i % 4) == 0;
      v1l.push_back(on);
      v3l.push_back(on);
   }
   REQUIRE(psio1::convert_to_bincode(v1l) ==
           psio::encode(psio::bincode{}, v3l));
}

TEST_CASE("bincode round-trip: bitlist",
          "[bincode][bitlist][round-trip]")
{
   psio::bitlist<256> in;
   for (int i = 0; i < 15; ++i)
      in.push_back(i % 2);
   auto bv = psio::encode(psio::bincode{}, in);
   auto out = psio::decode<psio::bitlist<256>>(
      psio::bincode{}, std::span<const char>{bv});
   REQUIRE(out.size() == in.size());
   for (std::size_t i = 0; i < in.size(); ++i)
      REQUIRE(out.test(i) == in.test(i));
}

TEST_CASE("bincode parity: uint128 + uint256", "[bincode][parity][extint]")
{
   psio1::uint128  v1u = (static_cast<psio1::uint128>(0x9) << 64) | 0xABC;
   psio::uint128 v3u = (static_cast<psio::uint128>(0x9) << 64) | 0xABC;
   REQUIRE(psio1::convert_to_bincode(v1u) ==
           psio::encode(psio::bincode{}, v3u));

   psio1::uint256 v1w; psio::uint256 v3w;
   for (int i = 0; i < 4; ++i)
   { v1w.limb[i] = 0xC000 + i; v3w.limb[i] = 0xC000 + i; }
   REQUIRE(psio1::convert_to_bincode(v1w) ==
           psio::encode(psio::bincode{}, v3w));
}
