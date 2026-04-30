// Tests for psio/msgpack.hpp — schema-driven MsgPack codec.
//
// Strategy: hand-craft byte sequences from the MsgPack spec for the
// scalar / container forms, assert encode produces those exact bytes
// (proving wire-format conformance) and decode round-trips back to
// the original C++ value.  Compound types (records, variants, nested
// containers) are checked for round-trip identity rather than exact
// bytes since their layout depends on field ordering.

#include <psio/msgpack.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using bytes_t = std::vector<char>;

namespace
{
   bytes_t b(std::initializer_list<unsigned> il)
   {
      bytes_t out;
      out.reserve(il.size());
      for (auto v : il)
         out.push_back(static_cast<char>(v & 0xff));
      return out;
   }

   template <typename T>
   void rt(const T& v)
   {
      auto enc = psio::encode(psio::msgpack{}, v);
      auto dec = psio::decode<T>(psio::msgpack{}, std::span<const char>{enc});
      REQUIRE(dec == v);
   }
}

// ─── nil + bool ──────────────────────────────────────────────────────

TEST_CASE("msgpack: bool tag bytes match spec", "[msgpack][scalar]")
{
   auto t = psio::encode(psio::msgpack{}, true);
   auto f = psio::encode(psio::msgpack{}, false);
   CHECK(t == b({0xc3}));
   CHECK(f == b({0xc2}));

   CHECK(psio::decode<bool>(psio::msgpack{}, std::span<const char>{t}));
   CHECK_FALSE(psio::decode<bool>(psio::msgpack{}, std::span<const char>{f}));
}

// ─── Integer width selection ─────────────────────────────────────────

TEST_CASE("msgpack: positive fixint (0..127) takes 1 byte",
          "[msgpack][int]")
{
   CHECK(psio::encode(psio::msgpack{}, std::uint32_t{0})   == b({0x00}));
   CHECK(psio::encode(psio::msgpack{}, std::uint32_t{1})   == b({0x01}));
   CHECK(psio::encode(psio::msgpack{}, std::uint32_t{127}) == b({0x7f}));
   rt(std::uint32_t{0});
   rt(std::uint32_t{1});
   rt(std::uint32_t{127});
}

TEST_CASE("msgpack: negative fixint (-32..-1) takes 1 byte",
          "[msgpack][int]")
{
   CHECK(psio::encode(psio::msgpack{}, std::int32_t{-1})  == b({0xff}));
   CHECK(psio::encode(psio::msgpack{}, std::int32_t{-32}) == b({0xe0}));
   rt(std::int32_t{-1});
   rt(std::int32_t{-32});
}

TEST_CASE("msgpack: u8 / u16 / u32 / u64 boundaries pick smallest form",
          "[msgpack][int]")
{
   //  128 → uint8  (0xcc 0x80)
   CHECK(psio::encode(psio::msgpack{}, std::uint32_t{128})
         == b({0xcc, 0x80}));
   //  256 → uint16 (0xcd 0x01 0x00)
   CHECK(psio::encode(psio::msgpack{}, std::uint32_t{256})
         == b({0xcd, 0x01, 0x00}));
   //  65536 → uint32 (0xce 0x00 0x01 0x00 0x00)
   CHECK(psio::encode(psio::msgpack{}, std::uint64_t{65536})
         == b({0xce, 0x00, 0x01, 0x00, 0x00}));
   //  4294967296 → uint64
   CHECK(psio::encode(psio::msgpack{}, std::uint64_t{4294967296ull})
         == b({0xcf, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}));

   rt(std::uint32_t{128});
   rt(std::uint32_t{256});
   rt(std::uint64_t{65536});
   rt(std::uint64_t{4294967296ull});
}

TEST_CASE("msgpack: negative ints pick smallest signed form",
          "[msgpack][int]")
{
   //  -33 → int8 (0xd0 0xdf)
   CHECK(psio::encode(psio::msgpack{}, std::int32_t{-33})
         == b({0xd0, 0xdf}));
   //  -128 → int8 (0xd0 0x80)
   CHECK(psio::encode(psio::msgpack{}, std::int32_t{-128})
         == b({0xd0, 0x80}));
   //  -129 → int16 (0xd1 0xff 0x7f)
   CHECK(psio::encode(psio::msgpack{}, std::int32_t{-129})
         == b({0xd1, 0xff, 0x7f}));

   rt(std::int32_t{-33});
   rt(std::int32_t{-128});
   rt(std::int32_t{-129});
   rt(std::int64_t{-2147483649ll});
}

// ─── Float ───────────────────────────────────────────────────────────

TEST_CASE("msgpack: f32 / f64 round-trip", "[msgpack][float]")
{
   rt(1.5f);
   rt(-3.14f);
   rt(2.71828);
   rt(-1e100);

   // f32 always emits 5 bytes; f64 always emits 9.
   CHECK(psio::encode(psio::msgpack{}, 0.0f).size()  == 5);
   CHECK(psio::encode(psio::msgpack{}, 0.0).size()   == 9);
   CHECK(psio::encode(psio::msgpack{}, 1.5f)[0]  == static_cast<char>(0xca));
   CHECK(psio::encode(psio::msgpack{}, 1.5)[0]   == static_cast<char>(0xcb));
}

// ─── String ──────────────────────────────────────────────────────────

TEST_CASE("msgpack: empty string is fixstr", "[msgpack][str]")
{
   CHECK(psio::encode(psio::msgpack{}, std::string{}) == b({0xa0}));
   rt(std::string{});
}

TEST_CASE("msgpack: short string uses fixstr (≤31)", "[msgpack][str]")
{
   auto enc = psio::encode(psio::msgpack{}, std::string{"hello"});
   //  fixstr length 5 → tag 0xa5 + "hello"
   REQUIRE(enc.size() == 6);
   CHECK(static_cast<unsigned char>(enc[0]) == 0xa5);
   CHECK(std::string(enc.data() + 1, 5) == "hello");
   rt(std::string{"hello"});
}

TEST_CASE("msgpack: 32-byte string tips into str8", "[msgpack][str]")
{
   std::string s(32, 'a');
   auto        enc = psio::encode(psio::msgpack{}, s);
   //  str8 → 0xd9 0x20 + 32 bytes
   REQUIRE(enc.size() == 34);
   CHECK(static_cast<unsigned char>(enc[0]) == 0xd9);
   CHECK(static_cast<unsigned char>(enc[1]) == 32);
   rt(s);
}

TEST_CASE("msgpack: 256-byte string tips into str16", "[msgpack][str]")
{
   std::string s(256, 'x');
   auto        enc = psio::encode(psio::msgpack{}, s);
   REQUIRE(enc.size() == 259);
   CHECK(static_cast<unsigned char>(enc[0]) == 0xda);
   CHECK(static_cast<unsigned char>(enc[1]) == 0x01);
   CHECK(static_cast<unsigned char>(enc[2]) == 0x00);
   rt(s);
}

// ─── Optional → nil OR T ─────────────────────────────────────────────

TEST_CASE("msgpack: optional none/some round-trip", "[msgpack][optional]")
{
   std::optional<std::int32_t> empty;
   std::optional<std::int32_t> some{42};

   auto e_enc = psio::encode(psio::msgpack{}, empty);
   CHECK(e_enc == b({0xc0}));
   rt(empty);
   rt(some);
   rt(std::optional<std::string>{"abc"});
}

// ─── Vector / array ──────────────────────────────────────────────────

TEST_CASE("msgpack: empty vector is fixarray(0)", "[msgpack][array]")
{
   std::vector<std::int32_t> v;
   CHECK(psio::encode(psio::msgpack{}, v) == b({0x90}));
   rt(v);
}

TEST_CASE("msgpack: small vector uses fixarray (≤15)",
          "[msgpack][array]")
{
   std::vector<std::int32_t> v{1, 2, 3};
   auto                      enc = psio::encode(psio::msgpack{}, v);
   //  fixarray(3) + three positive fixints
   CHECK(enc == b({0x93, 0x01, 0x02, 0x03}));
   rt(v);
}

TEST_CASE("msgpack: 16-element vector tips into array16",
          "[msgpack][array]")
{
   std::vector<std::int32_t> v(16, 1);
   auto                      enc = psio::encode(psio::msgpack{}, v);
   //  array16 + u16 length + 16 fixints
   REQUIRE(enc.size() == 19);
   CHECK(static_cast<unsigned char>(enc[0]) == 0xdc);
   CHECK(static_cast<unsigned char>(enc[1]) == 0x00);
   CHECK(static_cast<unsigned char>(enc[2]) == 0x10);
   rt(v);
}

TEST_CASE("msgpack: std::array round-trips with arity check",
          "[msgpack][array]")
{
   std::array<std::int32_t, 4> a{1, 2, 3, 4};
   rt(a);
}

// ─── PSIO_REFLECT'd record encodes as msgpack array ──────────────────

namespace test_msgpack
{
   struct Point
   {
      std::int32_t x;
      std::int32_t y;
   };
   PSIO_REFLECT(Point, x, y)

   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;
      std::optional<std::int32_t> count;
   };
   PSIO_REFLECT(Bag, name, payload, count)

   bool operator==(const Point& a, const Point& b)
   {
      return a.x == b.x && a.y == b.y;
   }
   bool operator==(const Bag& a, const Bag& b)
   {
      return a.name == b.name && a.payload == b.payload && a.count == b.count;
   }
}  // namespace test_msgpack

using test_msgpack::Bag;
using test_msgpack::Point;

TEST_CASE("msgpack: PSIO_REFLECT'd struct encodes as fixarray",
          "[msgpack][record]")
{
   Point p{7, -3};
   auto  enc = psio::encode(psio::msgpack{}, p);
   //  fixarray(2) + fixint(7) + neg fixint(-3 = 0xfd)
   CHECK(enc == b({0x92, 0x07, 0xfd}));
   auto dec = psio::decode<Point>(psio::msgpack{}, std::span<const char>{enc});
   CHECK(dec == p);
}

TEST_CASE("msgpack: nested record with vector + optional round-trips",
          "[msgpack][record]")
{
   Bag a{"alice", {1, 2, 3}, std::optional<std::int32_t>{42}};
   Bag b_no_count{"bob", {}, std::nullopt};

   rt(a);
   rt(b_no_count);
}

// ─── std::variant as 2-element [discriminator, value] array ──────────

TEST_CASE("msgpack: std::variant round-trips through 2-element array",
          "[msgpack][variant]")
{
   using V = std::variant<std::int32_t, std::string, bool>;

   V v0{std::in_place_index<0>, 42};
   V v1{std::in_place_index<1>, std::string{"hi"}};
   V v2{std::in_place_index<2>, true};

   rt(v0);
   rt(v1);
   rt(v2);

   // Wire shape: fixarray(2) + uint(idx) + value
   auto enc0 = psio::encode(psio::msgpack{}, v0);
   //  0x92, 0x00 (idx=0), 0x2a (42 as fixint)
   CHECK(enc0 == b({0x92, 0x00, 0x2a}));
}

// ─── validate / validate_strict ──────────────────────────────────────

TEST_CASE("msgpack: validate detects truncated input",
          "[msgpack][validate]")
{
   //  fixstr(5) + only 3 bytes
   auto bad = b({0xa5, 'h', 'e', 'l'});
   auto st  = psio::validate<std::string>(psio::msgpack{},
                                         std::span<const char>{bad});
   CHECK(!st);
}

TEST_CASE("msgpack: validate_strict rejects trailing bytes",
          "[msgpack][validate]")
{
   auto enc = psio::encode(psio::msgpack{}, std::string{"hi"});
   enc.push_back(0x7f);
   auto st = psio::validate_strict<std::string>(psio::msgpack{},
                                                std::span<const char>{enc});
   CHECK(!st);
}

TEST_CASE("msgpack: validate accepts well-formed input",
          "[msgpack][validate]")
{
   auto enc = psio::encode(psio::msgpack{}, std::string{"hello"});
   auto st  = psio::validate_strict<std::string>(psio::msgpack{},
                                                std::span<const char>{enc});
   CHECK(static_cast<bool>(st));
}

// ─── vector<uint8_t> routes to bin, not array ────────────────────────

TEST_CASE("msgpack: empty vector<u8> emits bin8(0)", "[msgpack][bin]")
{
   std::vector<std::uint8_t> v;
   //  bin8(0): 0xc4, 0x00
   CHECK(psio::encode(psio::msgpack{}, v) == b({0xc4, 0x00}));
   rt(v);
}

TEST_CASE("msgpack: small vector<u8> emits bin8 + payload",
          "[msgpack][bin]")
{
   std::vector<std::uint8_t> v{0x01, 0x02, 0x03};
   auto                      enc = psio::encode(psio::msgpack{}, v);
   //  bin8(3): 0xc4, 0x03 + 3 raw bytes
   CHECK(enc == b({0xc4, 0x03, 0x01, 0x02, 0x03}));
   rt(v);
}

TEST_CASE("msgpack: vector<u8> with high bytes — bin shows its size win",
          "[msgpack][bin]")
{
   //  16 bytes including values ≥ 0x80.  The fixarray-of-fixints path
   //  would inflate every ≥0x80 byte to a 2-byte uint8 form
   //  (1 + 8*1 + 8*2 = 25 bytes).  bin8 stays flat at 18.
   std::vector<std::uint8_t> v{
      0x00, 0x10, 0x7f, 0x80, 0x81, 0xa0, 0xc0, 0xff,
      0x33, 0x44, 0x55, 0xee, 0xdd, 0xcc, 0xbb, 0xaa};
   auto enc = psio::encode(psio::msgpack{}, v);
   REQUIRE(enc.size() == 18);
   CHECK(static_cast<unsigned char>(enc[0]) == 0xc4);  // bin8
   CHECK(static_cast<unsigned char>(enc[1]) == 0x10);  // length 16
   //  Body is a verbatim memcpy of the input.
   for (std::size_t i = 0; i < v.size(); ++i)
      CHECK(static_cast<unsigned char>(enc[2 + i]) == v[i]);
   rt(v);
}

TEST_CASE("msgpack: 256-byte vector<u8> tips into bin16",
          "[msgpack][bin]")
{
   std::vector<std::uint8_t> v(256, 0xAA);
   auto                      enc = psio::encode(psio::msgpack{}, v);
   //  bin16: 0xc5, u16-be length, payload
   REQUIRE(enc.size() == 3 + 256);
   CHECK(static_cast<unsigned char>(enc[0]) == 0xc5);
   CHECK(static_cast<unsigned char>(enc[1]) == 0x01);
   CHECK(static_cast<unsigned char>(enc[2]) == 0x00);
   rt(v);
}

TEST_CASE("msgpack: decode accepts back-compat array form for vector<u8>",
          "[msgpack][bin]")
{
   //  Hand-craft a fixarray(3) of fixints — what an older encoder
   //  (or our previous version) would have produced.
   auto legacy = b({0x93, 0x01, 0x02, 0x03});
   auto out    = psio::decode<std::vector<std::uint8_t>>(
      psio::msgpack{}, std::span<const char>{legacy});
   REQUIRE(out.size() == 3);
   CHECK(out[0] == 1);
   CHECK(out[1] == 2);
   CHECK(out[2] == 3);
}

TEST_CASE("msgpack: vector<int8_t> stays as array (signed semantics)",
          "[msgpack][bin]")
{
   //  vector<int8_t> represents "list of small signed ints", not
   //  a binary blob, so it should NOT route to bin.
   std::vector<std::int8_t> v{-1, 0, 1};
   auto                     enc = psio::encode(psio::msgpack{}, v);
   //  fixarray(3) + neg-fixint(-1=0xff) + posfixint(0) + posfixint(1)
   CHECK(enc == b({0x93, 0xff, 0x00, 0x01}));
}

// ─── Decode rejects type mismatch (one cross-check) ──────────────────

TEST_CASE("msgpack: decoding a string as int fails cleanly",
          "[msgpack][error]")
{
   auto enc = psio::encode(psio::msgpack{}, std::string{"hello"});
   bool threw = false;
   try
   {
      (void)psio::decode<std::int32_t>(psio::msgpack{},
                                       std::span<const char>{enc});
   }
   catch (const std::runtime_error&)
   {
      threw = true;
   }
   CHECK(threw);
}
