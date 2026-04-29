// v1 ↔ psio SSZ byte-parity tests.
//
// Links psio (v1) and psio into the same binary. For each fixture,
// encodes the same value through both codecs and asserts byte-identity.
// This is the design-doc-required gate (§6 / §8.5) that psio can
// actually replace psio for SSZ without changing downstream byte
// streams.

// Reflection macros live in the type's enclosing namespace, so if the
// same struct is reflected by both psio and psio it needs BOTH macros
// — we'd rather define it once and reflect with psio only, then
// template-cast to call v1's encoder. v1's encoder doesn't need v1
// reflection for non-record types; for records we define matching
// "twin" structs (one for each codec) with identical layout.

#include <psio1/bitset.hpp>     // v1 bitvector/bitlist types
#include <psio1/ext_int.hpp>    // v1 uint128/int128/uint256
#include <psio1/from_ssz.hpp>   // v1
#include <psio1/to_ssz.hpp>     // v1
#include <psio1/reflect.hpp>    // v1 PSIO1_REFLECT
#include <psio/ext_int.hpp>   // v3 uint128/int128/uint256
#include <psio/reflect.hpp>   // v3 PSIO_REFLECT
#include <psio/ssz.hpp>       // v3
#include <psio/wrappers.hpp>  // v3 bitvector/bitlist

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// ── Helpers to run both sides and diff ────────────────────────────────────

namespace {

   template <typename T>
   std::vector<char> v1_ssz(const T& v)
   {
      return psio1::convert_to_ssz(v);
   }

   template <typename T>
   std::vector<char> v3_ssz(const T& v)
   {
      return psio::encode(psio::ssz{}, v);
   }

   std::string hex_dump(const std::vector<char>& b)
   {
      std::string out;
      out.reserve(b.size() * 3);
      char buf[4];
      for (auto c : b)
      {
         std::snprintf(buf, sizeof(buf), "%02x ",
                       static_cast<unsigned>(static_cast<unsigned char>(c)));
         out += buf;
      }
      return out;
   }

}  // namespace

// ── Primitives ────────────────────────────────────────────────────────────

TEST_CASE("ssz parity: bool", "[ssz][parity]")
{
   REQUIRE(v1_ssz(true)  == v3_ssz(true));
   REQUIRE(v1_ssz(false) == v3_ssz(false));
}

TEST_CASE("ssz parity: integer primitives", "[ssz][parity]")
{
   REQUIRE(v1_ssz(std::uint8_t{0xAB})          == v3_ssz(std::uint8_t{0xAB}));
   REQUIRE(v1_ssz(std::uint16_t{0xCAFE})       == v3_ssz(std::uint16_t{0xCAFE}));
   REQUIRE(v1_ssz(std::uint32_t{0xDEADBEEF})   == v3_ssz(std::uint32_t{0xDEADBEEF}));
   REQUIRE(v1_ssz(std::uint64_t{0x1122334455667788ULL}) ==
           v3_ssz(std::uint64_t{0x1122334455667788ULL}));
   REQUIRE(v1_ssz(std::int32_t{-12345})        == v3_ssz(std::int32_t{-12345}));
   REQUIRE(v1_ssz(std::int16_t{-1})            == v3_ssz(std::int16_t{-1}));
}

TEST_CASE("ssz parity: float and double", "[ssz][parity]")
{
   REQUIRE(v1_ssz(3.14159f)             == v3_ssz(3.14159f));
   REQUIRE(v1_ssz(2.718281828459045)    == v3_ssz(2.718281828459045));
}

// ── Containers ────────────────────────────────────────────────────────────

TEST_CASE("ssz parity: std::array of fixed elements", "[ssz][parity]")
{
   std::array<std::uint32_t, 4> arr{1, 2, 3, 0x80000000};
   REQUIRE(v1_ssz(arr) == v3_ssz(arr));
}

TEST_CASE("ssz parity: std::vector of fixed elements", "[ssz][parity]")
{
   std::vector<std::uint32_t> v{10, 20, 30, 40, 50};
   REQUIRE(v1_ssz(v) == v3_ssz(v));
}

TEST_CASE("ssz parity: std::vector empty", "[ssz][parity]")
{
   std::vector<std::uint32_t> v;
   REQUIRE(v1_ssz(v) == v3_ssz(v));
}

TEST_CASE("ssz parity: std::string", "[ssz][parity]")
{
   std::string s = "hello world";
   REQUIRE(v1_ssz(s) == v3_ssz(s));
}

TEST_CASE("ssz parity: empty std::string", "[ssz][parity]")
{
   std::string s;
   REQUIRE(v1_ssz(s) == v3_ssz(s));
}

// ── std::optional (Union[null, T]) ────────────────────────────────────────

TEST_CASE("ssz parity: std::optional<u32> None", "[ssz][parity][optional]")
{
   std::optional<std::uint32_t> o;
   auto a = v1_ssz(o);
   auto b = v3_ssz(o);
   INFO("v1: " << hex_dump(a));
   INFO("v3: " << hex_dump(b));
   REQUIRE(a == b);
}

TEST_CASE("ssz parity: std::optional<u32> Some", "[ssz][parity][optional]")
{
   std::optional<std::uint32_t> o = 0xDEADBEEF;
   auto a = v1_ssz(o);
   auto b = v3_ssz(o);
   INFO("v1: " << hex_dump(a));
   INFO("v3: " << hex_dump(b));
   REQUIRE(a == b);
}

TEST_CASE("ssz parity: std::optional<string> Some", "[ssz][parity][optional]")
{
   std::optional<std::string> o = std::string("hello");
   auto a = v1_ssz(o);
   auto b = v3_ssz(o);
   INFO("v1: " << hex_dump(a));
   INFO("v3: " << hex_dump(b));
   REQUIRE(a == b);
}

// ── Reflected records ────────────────────────────────────────────────────
//
// Twin structs with identical layout — one reflected with PSIO1_REFLECT
// (v1), one with PSIO_REFLECT (v3). Encoder runs on each type and
// bytes are compared.

// Fixed record — reflected in its own namespace on each side.
namespace v1_types {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO1_REFLECT(Point, x, y)
}

namespace v3_types {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO_REFLECT(Point, x, y)
}

TEST_CASE("ssz parity: fixed record (Point)",
          "[ssz][parity][record]")
{
   v1_types::Point a{3, -7};
   v3_types::Point b{3, -7};

   auto av = psio1::convert_to_ssz(a);
   auto bv = psio::encode(psio::ssz{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

// Variable record (fixed field + string + vector + fixed field).
namespace v1_types {
   struct Blob {
      std::uint16_t            version;
      std::vector<std::uint32_t> payload;
      std::string              note;
   };
   PSIO1_REFLECT(Blob, version, payload, note)
}

namespace v3_types {
   struct Blob {
      std::uint16_t            version;
      std::vector<std::uint32_t> payload;
      std::string              note;
   };
   PSIO_REFLECT(Blob, version, payload, note)
}

TEST_CASE("ssz parity: variable record (Blob)",
          "[ssz][parity][record][variable]")
{
   v1_types::Blob a{7, {100, 200, 300}, "label"};
   v3_types::Blob b{7, {100, 200, 300}, "label"};

   auto av = psio1::convert_to_ssz(a);
   auto bv = psio::encode(psio::ssz{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("ssz parity: variable record with empty variable fields",
          "[ssz][parity][record][variable]")
{
   v1_types::Blob a{0, {}, ""};
   v3_types::Blob b{0, {}, ""};

   auto av = psio1::convert_to_ssz(a);
   auto bv = psio::encode(psio::ssz{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

// ── Vector of variable-size elements (List of Lists) ──────────────────────

TEST_CASE("ssz parity: vector<string>",
          "[ssz][parity][vector][variable]")
{
   std::vector<std::string> v{"alpha", "beta", "gamma"};
   auto av = v1_ssz(v);
   auto bv = v3_ssz(v);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("ssz parity: vector<vector<u32>>",
          "[ssz][parity][vector][variable]")
{
   std::vector<std::vector<std::uint32_t>> v{{1, 2}, {}, {3, 4, 5}};
   auto av = v1_ssz(v);
   auto bv = v3_ssz(v);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

// ── Nested record (variable record inside a variable record) ─────────────

namespace v1_types {
   struct Nested {
      Blob              inner;
      std::uint64_t     tail;
   };
   PSIO1_REFLECT(Nested, inner, tail)
}
namespace v3_types {
   struct Nested {
      Blob              inner;
      std::uint64_t     tail;
   };
   PSIO_REFLECT(Nested, inner, tail)
}

TEST_CASE("ssz parity: nested variable record",
          "[ssz][parity][record][nested]")
{
   v1_types::Nested a{{1, {10, 20}, "inner"}, 999};
   v3_types::Nested b{{1, {10, 20}, "inner"}, 999};

   auto av = psio1::convert_to_ssz(a);
   auto bv = psio::encode(psio::ssz{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

// ── Round-trip parity: encode with one, decode with the other ─────────────

TEST_CASE("ssz parity: v1 encode → v3 decode (primitive)",
          "[ssz][parity][round-trip]")
{
   std::uint32_t v = 0xCAFEBABE;
   auto          av = psio1::convert_to_ssz(v);
   auto          back =
      psio::decode<std::uint32_t>(psio::ssz{}, std::span<const char>{av});
   REQUIRE(back == v);
}

TEST_CASE("ssz parity: v3 encode → v1 decode (primitive)",
          "[ssz][parity][round-trip]")
{
   std::uint64_t v = 0x1234567890ABCDEFULL;
   auto          bv = psio::encode(psio::ssz{}, v);
   auto back = psio1::convert_from_ssz<std::uint64_t>(bv);
   REQUIRE(back == v);
}

TEST_CASE("ssz parity: v1 encode → v3 decode (variable record)",
          "[ssz][parity][round-trip][record]")
{
   v1_types::Blob a{42, {1, 2, 3}, "vv"};
   auto           av = psio1::convert_to_ssz(a);

   auto back = psio::decode<v3_types::Blob>(
      psio::ssz{}, std::span<const char>{av});
   REQUIRE(back.version == 42);
   REQUIRE(back.payload == std::vector<std::uint32_t>{1, 2, 3});
   REQUIRE(back.note == "vv");
}

// ── bitvector / bitlist ───────────────────────────────────────────────

TEST_CASE("ssz parity: bitvector<16>",
          "[ssz][parity][bitvector]")
{
   psio1::bitvector<16>  v1v;
   psio::bitvector<16> v3v;
   for (std::size_t i : {0, 3, 8, 15})
   {
      v1v.set(i, true);
      v3v.set(i, true);
   }
   REQUIRE(psio1::convert_to_ssz(v1v) ==
           psio::encode(psio::ssz{}, v3v));
}

TEST_CASE("ssz parity: bitvector<13> (non-byte-aligned)",
          "[ssz][parity][bitvector]")
{
   psio1::bitvector<13>  v1v;
   psio::bitvector<13> v3v;
   for (std::size_t i : {0, 5, 12})
   {
      v1v.set(i, true);
      v3v.set(i, true);
   }
   REQUIRE(psio1::convert_to_ssz(v1v) ==
           psio::encode(psio::ssz{}, v3v));
}

TEST_CASE("ssz parity: bitlist<256>", "[ssz][parity][bitlist]")
{
   psio1::bitlist<256>  v1l;
   psio::bitlist<256> v3l;
   for (int i = 0; i < 20; ++i)
   {
      bool on = (i % 3) == 0;
      v1l.push_back(on);
      v3l.push_back(on);
   }
   REQUIRE(psio1::convert_to_ssz(v1l) ==
           psio::encode(psio::ssz{}, v3l));
}

TEST_CASE("ssz parity: empty bitlist", "[ssz][parity][bitlist]")
{
   psio1::bitlist<256>  v1l;
   psio::bitlist<256> v3l;
   REQUIRE(psio1::convert_to_ssz(v1l) ==
           psio::encode(psio::ssz{}, v3l));
}

TEST_CASE("ssz round-trip: bitvector<32>",
          "[ssz][bitvector][round-trip]")
{
   psio::bitvector<32> in;
   for (std::size_t i : {0, 7, 16, 31})
      in.set(i, true);
   auto bv = psio::encode(psio::ssz{}, in);
   auto out = psio::decode<psio::bitvector<32>>(
      psio::ssz{}, std::span<const char>{bv});
   for (std::size_t i = 0; i < 32; ++i)
      REQUIRE(out.test(i) == in.test(i));
}

TEST_CASE("ssz round-trip: bitlist<128>",
          "[ssz][bitlist][round-trip]")
{
   psio::bitlist<128> in;
   for (int i = 0; i < 10; ++i)
      in.push_back(i % 2 == 0);
   auto bv  = psio::encode(psio::ssz{}, in);
   auto out = psio::decode<psio::bitlist<128>>(
      psio::ssz{}, std::span<const char>{bv});
   REQUIRE(out.size() == in.size());
   for (std::size_t i = 0; i < in.size(); ++i)
      REQUIRE(out.test(i) == in.test(i));
}

// ── uint128 / int128 / uint256 parity ─────────────────────────────────

TEST_CASE("ssz parity: uint128", "[ssz][parity][extint]")
{
   psio1::uint128  v1v = (static_cast<psio1::uint128>(0x1234567890ABCDEFULL)
                         << 64) |
                        0xFEDCBA9876543210ULL;
   psio::uint128 v3v = (static_cast<psio::uint128>(0x1234567890ABCDEFULL)
                         << 64) |
                        0xFEDCBA9876543210ULL;
   REQUIRE(psio1::convert_to_ssz(v1v) ==
           psio::encode(psio::ssz{}, v3v));
}

TEST_CASE("ssz parity: int128 negative", "[ssz][parity][extint]")
{
   psio1::int128  v1v = -(static_cast<psio1::int128>(1) << 100);
   psio::int128 v3v = -(static_cast<psio::int128>(1) << 100);
   REQUIRE(psio1::convert_to_ssz(v1v) ==
           psio::encode(psio::ssz{}, v3v));
}

TEST_CASE("ssz parity: uint256", "[ssz][parity][extint]")
{
   psio1::uint256  v1v;
   psio::uint256 v3v;
   for (int i = 0; i < 4; ++i)
   {
      v1v.limb[i] = static_cast<std::uint64_t>(0xA000 + i);
      v3v.limb[i] = static_cast<std::uint64_t>(0xA000 + i);
   }
   REQUIRE(psio1::convert_to_ssz(v1v) ==
           psio::encode(psio::ssz{}, v3v));
}

TEST_CASE("ssz round-trip: uint256", "[ssz][extint][round-trip]")
{
   psio::uint256 in;
   in.limb[0] = 0xDEADBEEFCAFEBABE;
   in.limb[1] = 0x1234567890ABCDEF;
   in.limb[2] = 0x0F0F0F0F0F0F0F0F;
   in.limb[3] = 0xFFFFFFFFFFFFFFFF;
   auto bv  = psio::encode(psio::ssz{}, in);
   auto out = psio::decode<psio::uint256>(
      psio::ssz{}, std::span<const char>{bv});
   REQUIRE(out == in);
}
