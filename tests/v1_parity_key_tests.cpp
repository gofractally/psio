// v1 ↔ psio key byte-parity tests.
//
// key wire (MVP scope):
//   unsigned integral: big-endian
//   signed integral:   sign-flipped, big-endian
//   floats:            IEEE-754 sign-transform + big-endian
//   bool:              1 byte
//   strings / octet vectors: escape \0→\0\1 + \0\0 terminator
//   non-octet vectors: \1 per element + \0 terminator
//   optional:          \0 or \1 + value
//   record:            fields concatenated

#include <psio1/to_key.hpp>
#include <psio1/from_key.hpp>
#include <psio1/reflect.hpp>
#include <psio/key.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("key parity: unsigned integrals", "[key][parity][primitive]")
{
   REQUIRE(psio1::convert_to_key(std::uint32_t{0xDEADBEEF}) ==
           psio::encode(psio::key{}, std::uint32_t{0xDEADBEEF}));
   REQUIRE(psio1::convert_to_key(std::uint64_t{0x1122334455667788ULL}) ==
           psio::encode(psio::key{},
                         std::uint64_t{0x1122334455667788ULL}));
   REQUIRE(psio1::convert_to_key(std::uint8_t{0xAB}) ==
           psio::encode(psio::key{}, std::uint8_t{0xAB}));
}

TEST_CASE("key parity: signed integrals", "[key][parity][primitive]")
{
   REQUIRE(psio1::convert_to_key(std::int32_t{-123}) ==
           psio::encode(psio::key{}, std::int32_t{-123}));
   REQUIRE(psio1::convert_to_key(std::int32_t{12345678}) ==
           psio::encode(psio::key{}, std::int32_t{12345678}));
   REQUIRE(psio1::convert_to_key(std::int64_t{-1LL << 62}) ==
           psio::encode(psio::key{}, std::int64_t{-1LL << 62}));
}

TEST_CASE("key parity: bool", "[key][parity][primitive]")
{
   REQUIRE(psio1::convert_to_key(true) ==
           psio::encode(psio::key{}, true));
   REQUIRE(psio1::convert_to_key(false) ==
           psio::encode(psio::key{}, false));
}

TEST_CASE("key parity: string", "[key][parity][string]")
{
   REQUIRE(psio1::convert_to_key(std::string("hello")) ==
           psio::encode(psio::key{}, std::string("hello")));
   REQUIRE(psio1::convert_to_key(std::string{}) ==
           psio::encode(psio::key{}, std::string{}));
   std::string with_null = std::string("foo\0bar", 7);
   REQUIRE(psio1::convert_to_key(with_null) ==
           psio::encode(psio::key{}, with_null));
}

TEST_CASE("key parity: optional<u32>", "[key][parity][optional]")
{
   std::optional<std::uint32_t> n;
   std::optional<std::uint32_t> s = 0xDEADBEEF;
   REQUIRE(psio1::convert_to_key(n) == psio::encode(psio::key{}, n));
   REQUIRE(psio1::convert_to_key(s) == psio::encode(psio::key{}, s));
}

TEST_CASE("key parity: vector<u32> (non-octet)", "[key][parity][vector]")
{
   std::vector<std::uint32_t> v{10, 20, 30};
   REQUIRE(psio1::convert_to_key(v) == psio::encode(psio::key{}, v));
   std::vector<std::uint32_t> empty;
   REQUIRE(psio1::convert_to_key(empty) ==
           psio::encode(psio::key{}, empty));
}

TEST_CASE("key parity: vector<u8> (octet)", "[key][parity][vector]")
{
   std::vector<std::uint8_t> v{0, 1, 2, 0, 255};
   REQUIRE(psio1::convert_to_key(v) == psio::encode(psio::key{}, v));
}

namespace v1_key {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO1_REFLECT(Point, x, y)

   struct Blob { std::uint16_t v; std::string s; };
   PSIO1_REFLECT(Blob, v, s)
}
namespace v3_key {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO_REFLECT(Point, x, y)

   struct Blob { std::uint16_t v; std::string s; };
   PSIO_REFLECT(Blob, v, s)
}

TEST_CASE("key parity: fixed record", "[key][parity][record]")
{
   v1_key::Point a{3, -7};
   v3_key::Point b{3, -7};
   REQUIRE(psio1::convert_to_key(a) == psio::encode(psio::key{}, b));
}

TEST_CASE("key parity: variable record", "[key][parity][record]")
{
   v1_key::Blob a{7, "hi"};
   v3_key::Blob b{7, "hi"};
   REQUIRE(psio1::convert_to_key(a) == psio::encode(psio::key{}, b));
}

TEST_CASE("key round-trip: v3 encode → v3 decode", "[key][parity][round-trip]")
{
   v3_key::Blob b{42, "hello"};
   auto         bv = psio::encode(psio::key{}, b);
   auto         back =
      psio::decode<v3_key::Blob>(psio::key{}, std::span<const char>{bv});
   REQUIRE(back.v == 42);
   REQUIRE(back.s == "hello");
}

TEST_CASE("key parity: std::variant", "[key][parity][variant]")
{
   using V = std::variant<std::uint32_t, std::string>;
   REQUIRE(psio1::convert_to_key(V{std::uint32_t{0xDEADBEEF}}) ==
           psio::encode(psio::key{}, V{std::uint32_t{0xDEADBEEF}}));
   REQUIRE(psio1::convert_to_key(V{std::string("hi")}) ==
           psio::encode(psio::key{}, V{std::string("hi")}));
}

// v1 key has no built-in uint256 path; psio encodes it as big-endian
// MSB-first limb order for sortability. Round-trip only.

#include <psio/ext_int.hpp>

TEST_CASE("key round-trip: uint256 sortable",
          "[key][extint][round-trip]")
{
   psio::uint256 in;
   for (int i = 0; i < 4; ++i) in.limb[i] = 0xF000 + i;
   auto bv = psio::encode(psio::key{}, in);
   auto out = psio::decode<psio::uint256>(psio::key{},
                                            std::span<const char>{bv});
   REQUIRE(out == in);
}

TEST_CASE("key round-trip: uint128", "[key][extint][round-trip]")
{
   psio::uint128 in = (static_cast<psio::uint128>(0xDEADBEEFCAFEBABE) << 64)
                    | 0x1234567890ABCDEFULL;
   auto bv = psio::encode(psio::key{}, in);
   auto out = psio::decode<psio::uint128>(psio::key{},
                                            std::span<const char>{bv});
   REQUIRE(out == in);
}
