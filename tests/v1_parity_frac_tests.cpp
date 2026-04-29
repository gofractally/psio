// v1 ↔ psio fracpack byte-parity tests.
//
// Records use v1's pointer-relative offset scheme: in the fixed region
// each variable field holds a W-byte slot; the slot's value is
// (heap_pos − slot_pos). Special slot values:
//   0 → Some(empty container)  /  non-optional empty container
//   1 → None (optional only)
// Variable field heap payload is the type's own self-describing pack()
// output (e.g. string: [W-byte byte_count][bytes]).

#include <psio1/ext_int.hpp>
#include <psio1/fracpack.hpp>
#include <psio1/reflect.hpp>
#include <psio/ext_int.hpp>
#include <psio/frac.hpp>
#include <psio/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {
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

TEST_CASE("frac parity: primitives encode to raw LE bytes",
          "[frac][parity][primitive]")
{
   std::uint32_t v  = 0xDEADBEEF;
   auto          av = psio1::convert_to_frac(v);
   auto          bv = psio::encode(psio::frac32{}, v);
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: u64", "[frac][parity][primitive]")
{
   std::uint64_t v  = 0x1122334455667788ULL;
   auto          av = psio1::convert_to_frac(v);
   auto          bv = psio::encode(psio::frac32{}, v);
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: bool", "[frac][parity][primitive]")
{
   REQUIRE(psio1::convert_to_frac(true) ==
           psio::encode(psio::frac32{}, true));
   REQUIRE(psio1::convert_to_frac(false) ==
           psio::encode(psio::frac32{}, false));
}

TEST_CASE("frac parity: float / double", "[frac][parity][primitive]")
{
   REQUIRE(psio1::convert_to_frac(3.14159f) ==
           psio::encode(psio::frac32{}, 3.14159f));
   REQUIRE(psio1::convert_to_frac(2.718281828459045) ==
           psio::encode(psio::frac32{}, 2.718281828459045));
}

TEST_CASE("frac parity: std::array<u32, N>",
          "[frac][parity][array]")
{
   std::array<std::uint32_t, 4> arr{1, 2, 3, 4};
   REQUIRE(psio1::convert_to_frac(arr) ==
           psio::encode(psio::frac32{}, arr));
}

// ── Vectors / strings: byte-count prefix ─────────────────────────────────

TEST_CASE("frac parity: std::vector<u32>",
          "[frac][parity][vector]")
{
   std::vector<std::uint32_t> v{10, 20, 30, 40};
   auto                       av = psio1::convert_to_frac(v);
   auto                       bv = psio::encode(psio::frac32{}, v);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: empty std::vector",
          "[frac][parity][vector]")
{
   std::vector<std::uint32_t> v;
   REQUIRE(psio1::convert_to_frac(v) == psio::encode(psio::frac32{}, v));
}

TEST_CASE("frac parity: std::string", "[frac][parity][string]")
{
   std::string s = "hello world";
   auto        av = psio1::convert_to_frac(s);
   auto        bv = psio::encode(psio::frac32{}, s);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: empty std::string", "[frac][parity][string]")
{
   std::string s;
   REQUIRE(psio1::convert_to_frac(s) == psio::encode(psio::frac32{}, s));
}

// ── Optional (top-level) ─────────────────────────────────────────────────

TEST_CASE("frac parity: std::optional<u32> None → sentinel 1",
          "[frac][parity][optional]")
{
   std::optional<std::uint32_t> o;
   auto                         av = psio1::convert_to_frac(o);
   auto                         bv = psio::encode(psio::frac32{}, o);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: std::optional<u32> Some",
          "[frac][parity][optional]")
{
   std::optional<std::uint32_t> o = 0xDEADBEEF;
   auto                         av = psio1::convert_to_frac(o);
   auto                         bv = psio::encode(psio::frac32{}, o);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: std::optional<string> None",
          "[frac][parity][optional]")
{
   std::optional<std::string> o;
   REQUIRE(psio1::convert_to_frac(o) == psio::encode(psio::frac32{}, o));
}

TEST_CASE("frac parity: std::optional<string> Some",
          "[frac][parity][optional]")
{
   std::optional<std::string> o = std::string("hi");
   auto                       av = psio1::convert_to_frac(o);
   auto                       bv = psio::encode(psio::frac32{}, o);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

// ── Records ──────────────────────────────────────────────────────────────

namespace v1_frac {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO1_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t             version;
      std::vector<std::uint32_t> payload;
      std::string               note;
   };
   PSIO1_REFLECT(Blob, version, payload, note)
}
namespace v3_frac {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t             version;
      std::vector<std::uint32_t> payload;
      std::string               note;
   };
   PSIO_REFLECT(Blob, version, payload, note)
}

TEST_CASE("frac parity: fixed record (Point)",
          "[frac][parity][record]")
{
   v1_frac::Point a{3, -7};
   v3_frac::Point b{3, -7};
   auto           av = psio1::convert_to_frac(a);
   auto           bv = psio::encode(psio::frac32{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: variable record (Blob)",
          "[frac][parity][record][variable]")
{
   v1_frac::Blob a{7, {100, 200, 300}, "label"};
   v3_frac::Blob b{7, {100, 200, 300}, "label"};
   auto          av = psio1::convert_to_frac(a);
   auto          bv = psio::encode(psio::frac32{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

TEST_CASE("frac parity: variable record with empty containers",
          "[frac][parity][record][variable]")
{
   v1_frac::Blob a{0, {}, ""};
   v3_frac::Blob b{0, {}, ""};
   auto          av = psio1::convert_to_frac(a);
   auto          bv = psio::encode(psio::frac32{}, b);
   INFO("v1: " << hex_dump(av));
   INFO("v3: " << hex_dump(bv));
   REQUIRE(av == bv);
}

// ── Cross-decode: v1 encode → v3 decode and vice versa ──────────────────

TEST_CASE("frac parity: v1 encode → v3 decode (Point)",
          "[frac][parity][record][round-trip]")
{
   v1_frac::Point a{11, 13};
   auto           av = psio1::convert_to_frac(a);
   auto back = psio::decode<v3_frac::Point>(psio::frac32{},
                                              std::span<const char>{av});
   REQUIRE(back.x == 11);
   REQUIRE(back.y == 13);
}

TEST_CASE("frac parity: v1 encode → v3 decode (Blob)",
          "[frac][parity][record][round-trip]")
{
   v1_frac::Blob a{42, {1, 2, 3}, "xx"};
   auto          av = psio1::convert_to_frac(a);
   auto back = psio::decode<v3_frac::Blob>(psio::frac32{},
                                             std::span<const char>{av});
   REQUIRE(back.version == 42);
   REQUIRE(back.payload == std::vector<std::uint32_t>{1, 2, 3});
   REQUIRE(back.note == "xx");
}

TEST_CASE("frac parity: v3 encode → v1 decode (Blob)",
          "[frac][parity][record][round-trip]")
{
   v3_frac::Blob b{99, {7, 8}, "yo"};
   auto          bv = psio::encode(psio::frac32{}, b);
   v1_frac::Blob back;
   REQUIRE(psio1::from_frac<v1_frac::Blob>(
      back, std::span<const char>{bv}));
   REQUIRE(back.version == 99);
   REQUIRE(back.payload == std::vector<std::uint32_t>{7, 8});
   REQUIRE(back.note == "yo");
}

// ── std::variant (frac32 wire: u8 tag + u{W} size + payload) ───────────

TEST_CASE("frac parity: std::variant<u32, string>",
          "[frac][parity][variant]")
{
   using V = std::variant<std::uint32_t, std::string>;
   REQUIRE(psio1::convert_to_frac(V{std::uint32_t{0xDEADBEEF}}) ==
           psio::encode(psio::frac32{},
                         V{std::uint32_t{0xDEADBEEF}}));
   REQUIRE(psio1::convert_to_frac(V{std::string("xyz")}) ==
           psio::encode(psio::frac32{}, V{std::string("xyz")}));
}

TEST_CASE("frac round-trip: std::variant", "[frac][variant][round-trip]")
{
   using V = std::variant<std::uint32_t, std::string>;
   V      in = std::string("hello");
   auto   bv = psio::encode(psio::frac32{}, in);
   auto   out =
      psio::decode<V>(psio::frac32{}, std::span<const char>{bv});
   REQUIRE(out.index() == 1);
   REQUIRE(std::get<1>(out) == "hello");
}

// ── uint128 / uint256 ────────────────────────────────────────────────

TEST_CASE("frac parity: uint128", "[frac][parity][extint]")
{
   psio1::uint128  v1v =
      (static_cast<psio1::uint128>(0x1234567890ABCDEFULL) << 64) |
      0xFEDCBA9876543210ULL;
   psio::uint128 v3v =
      (static_cast<psio::uint128>(0x1234567890ABCDEFULL) << 64) |
      0xFEDCBA9876543210ULL;
   REQUIRE(psio1::convert_to_frac(v1v) ==
           psio::encode(psio::frac32{}, v3v));
}

TEST_CASE("frac parity: uint256", "[frac][parity][extint]")
{
   psio1::uint256  v1v;
   psio::uint256 v3v;
   for (int i = 0; i < 4; ++i)
   {
      v1v.limb[i] = 0xA000 + i;
      v3v.limb[i] = 0xA000 + i;
   }
   REQUIRE(psio1::convert_to_frac(v1v) ==
           psio::encode(psio::frac32{}, v3v));
}
