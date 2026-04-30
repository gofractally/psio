// Phase 6 — SSZ (Simple Serialize) format.
//
// Covers the MVP scope per the phase plan:
//   - primitives (bool, integers, float, double)
//   - std::array<T, N> of fixed + variable elements
//   - std::vector<T> of fixed + variable elements
//   - std::string
//   - std::optional<T>
//   - reflected records (fixed + variable)

#include <psio/reflect.hpp>
#include <psio/ssz.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── Fixed records ──────────────────────────────────────────────────────

struct Point2
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(Point2, x, y)

struct Nested
{
   Point2       origin;
   std::int16_t radius;
};
PSIO_REFLECT(Nested, origin, radius)

// ── Variable-field record ─────────────────────────────────────────────

struct Labelled
{
   std::int32_t id;
   std::string  name;
};
PSIO_REFLECT(Labelled, id, name)

struct TwoVars
{
   std::string a;
   std::string b;
};
PSIO_REFLECT(TwoVars, a, b)

// ── Record containing a vector ────────────────────────────────────────

struct Packet
{
   std::uint16_t          version;
   std::vector<std::int32_t> payload;
};
PSIO_REFLECT(Packet, version, payload)

// ────────────────────────────────────────────────────────────────────────
// Primitives
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz encodes bool as single 0x00 / 0x01 byte", "[ssz][primitive]")
{
   std::vector<char> out;
   psio::encode(psio::ssz{}, true, out);
   REQUIRE(out.size() == 1);
   REQUIRE(static_cast<unsigned char>(out[0]) == 0x01);

   out.clear();
   psio::encode(psio::ssz{}, false, out);
   REQUIRE(out.size() == 1);
   REQUIRE(static_cast<unsigned char>(out[0]) == 0x00);
}

TEST_CASE("ssz round-trips bool", "[ssz][primitive]")
{
   auto buf = psio::encode(psio::ssz{}, true);
   auto v   = psio::decode<bool>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(v);
}

TEST_CASE("ssz encodes integers little-endian, round-trips",
          "[ssz][primitive]")
{
   std::uint32_t v = 0x04030201;

   auto buf = psio::encode(psio::ssz{}, v);
   REQUIRE(buf.size() == 4);
   REQUIRE(static_cast<unsigned char>(buf[0]) == 0x01);
   REQUIRE(static_cast<unsigned char>(buf[1]) == 0x02);
   REQUIRE(static_cast<unsigned char>(buf[2]) == 0x03);
   REQUIRE(static_cast<unsigned char>(buf[3]) == 0x04);

   auto back = psio::decode<std::uint32_t>(psio::ssz{},
                                            std::span<const char>{buf});
   REQUIRE(back == 0x04030201);
}

TEST_CASE("ssz handles signed integers correctly", "[ssz][primitive]")
{
   std::int16_t v = -1;
   auto         buf =
      psio::encode(psio::ssz{}, v);
   REQUIRE(buf.size() == 2);
   auto back =
      psio::decode<std::int16_t>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back == -1);
}

TEST_CASE("ssz round-trips float and double", "[ssz][primitive]")
{
   float f = 3.14159f;
   auto  fbuf = psio::encode(psio::ssz{}, f);
   REQUIRE(fbuf.size() == 4);
   auto fback =
      psio::decode<float>(psio::ssz{}, std::span<const char>{fbuf});
   REQUIRE(fback == f);

   double d = 2.718281828;
   auto   dbuf = psio::encode(psio::ssz{}, d);
   REQUIRE(dbuf.size() == 8);
   auto dback =
      psio::decode<double>(psio::ssz{}, std::span<const char>{dbuf});
   REQUIRE(dback == d);
}

// ────────────────────────────────────────────────────────────────────────
// Fixed arrays & records
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz encodes std::array of fixed elements as packed bytes",
          "[ssz][array]")
{
   std::array<std::uint32_t, 3> arr{{1, 2, 3}};
   auto buf = psio::encode(psio::ssz{}, arr);
   REQUIRE(buf.size() == 12);

   auto back =
      psio::decode<std::array<std::uint32_t, 3>>(psio::ssz{},
                                                  std::span<const char>{buf});
   REQUIRE(back == arr);
}

TEST_CASE("ssz round-trips fixed records", "[ssz][record]")
{
   Point2 p{3, 5};
   auto   buf = psio::encode(psio::ssz{}, p);
   REQUIRE(buf.size() == 8);

   auto back =
      psio::decode<Point2>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.x == 3);
   REQUIRE(back.y == 5);
}

TEST_CASE("ssz round-trips nested fixed records", "[ssz][record]")
{
   Nested n{{7, 11}, -1};
   auto   buf = psio::encode(psio::ssz{}, n);
   REQUIRE(buf.size() == 4 + 4 + 2);

   auto back =
      psio::decode<Nested>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.origin.x == 7);
   REQUIRE(back.origin.y == 11);
   REQUIRE(back.radius == -1);
}

// ────────────────────────────────────────────────────────────────────────
// Variable types
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz round-trips std::vector of fixed elements", "[ssz][vector]")
{
   std::vector<std::uint32_t> v{1, 2, 3, 4, 5};
   auto                       buf = psio::encode(psio::ssz{}, v);
   REQUIRE(buf.size() == 20);

   auto back = psio::decode<std::vector<std::uint32_t>>(
      psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back == v);
}

TEST_CASE("ssz round-trips empty std::vector", "[ssz][vector]")
{
   std::vector<std::uint32_t> v;
   auto                       buf = psio::encode(psio::ssz{}, v);
   REQUIRE(buf.size() == 0);

   auto back = psio::decode<std::vector<std::uint32_t>>(
      psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.empty());
}

TEST_CASE("ssz round-trips std::string", "[ssz][string]")
{
   std::string s = "hello world";
   auto        buf = psio::encode(psio::ssz{}, s);
   REQUIRE(buf.size() == s.size());

   auto back =
      psio::decode<std::string>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back == s);
}

TEST_CASE("ssz encodes optional as Union[null, T]", "[ssz][optional]")
{
   // SSZ canonical Union encoding matches v1 psio:
   //   None    → 0x00
   //   Some(x) → 0x01 || serialized(x)
   std::optional<std::uint32_t> some = 42;
   auto                         buf  = psio::encode(psio::ssz{}, some);
   REQUIRE(buf.size() == 5);  // selector + 4-byte payload
   REQUIRE(static_cast<unsigned char>(buf[0]) == 0x01);

   auto back = psio::decode<std::optional<std::uint32_t>>(
      psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.has_value());
   REQUIRE(*back == 42);

   std::optional<std::uint32_t> none;
   auto                         empty = psio::encode(psio::ssz{}, none);
   REQUIRE(empty.size() == 1);
   REQUIRE(static_cast<unsigned char>(empty[0]) == 0x00);

   auto back_empty = psio::decode<std::optional<std::uint32_t>>(
      psio::ssz{}, std::span<const char>{empty});
   REQUIRE(!back_empty.has_value());
}

// ────────────────────────────────────────────────────────────────────────
// Variable records
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz round-trips record with one variable field",
          "[ssz][record][variable]")
{
   Labelled l{42, "alice"};
   auto     buf = psio::encode(psio::ssz{}, l);
   // Fixed region: 4 bytes id + 4 bytes offset = 8; tail "alice" = 5.
   REQUIRE(buf.size() == 13);

   auto back =
      psio::decode<Labelled>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.id == 42);
   REQUIRE(back.name == "alice");
}

TEST_CASE("ssz round-trips record with two variable fields",
          "[ssz][record][variable]")
{
   TwoVars tv{"abc", "defgh"};
   auto    buf = psio::encode(psio::ssz{}, tv);
   // Fixed region: 4 + 4 = 8; tail = 3 + 5 = 8.
   REQUIRE(buf.size() == 16);

   auto back =
      psio::decode<TwoVars>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.a == "abc");
   REQUIRE(back.b == "defgh");
}

TEST_CASE("ssz round-trips a record containing a vector",
          "[ssz][record][vector]")
{
   Packet p{3, {10, 20, 30}};
   auto   buf = psio::encode(psio::ssz{}, p);
   // version (2) + offset (4) + 3 x int32 = 6 + 12 = 18.
   REQUIRE(buf.size() == 18);

   auto back =
      psio::decode<Packet>(psio::ssz{}, std::span<const char>{buf});
   REQUIRE(back.version == 3);
   REQUIRE(back.payload == std::vector<std::int32_t>{10, 20, 30});
}

// ────────────────────────────────────────────────────────────────────────
// size_of
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz size_of returns wire byte length", "[ssz][size_of]")
{
   REQUIRE(psio::size_of(psio::ssz{}, std::uint32_t{0}) == 4);
   REQUIRE(psio::size_of(psio::ssz{}, true) == 1);
   REQUIRE(psio::size_of(psio::ssz{}, std::string{"hello"}) == 5);

   std::vector<std::uint32_t> v{1, 2, 3};
   REQUIRE(psio::size_of(psio::ssz{}, v) == 12);

   Labelled l{1, "abc"};
   REQUIRE(psio::size_of(psio::ssz{}, l) == 4 + 4 + 3);
}

// ────────────────────────────────────────────────────────────────────────
// validate
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz validate accepts well-formed buffers", "[ssz][validate]")
{
   auto buf = psio::encode(psio::ssz{}, std::uint32_t{42});
   auto s =
      psio::validate<std::uint32_t>(psio::ssz{},
                                     std::span<const char>{buf});
   REQUIRE(s.ok());
}

TEST_CASE("ssz validate rejects truncated primitive buffer",
          "[ssz][validate]")
{
   char tiny[2]{};
   auto s = psio::validate<std::uint32_t>(psio::ssz{},
                                           std::span<const char>{tiny, 2});
   REQUIRE(!s.ok());
   REQUIRE(s.error().format_name == "ssz");
}

TEST_CASE("ssz validate catches non-multiple vector length",
          "[ssz][validate]")
{
   char buf[5]{};
   auto s = psio::validate<std::vector<std::uint32_t>>(
      psio::ssz{}, std::span<const char>{buf, 5});
   REQUIRE(!s.ok());
}

// ────────────────────────────────────────────────────────────────────────
// format_tag_base scoped sugar
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("ssz::encode scoped sugar matches psio::encode",
          "[ssz][format_tag_base]")
{
   Point2 p{9, -3};
   auto   a = psio::encode(psio::ssz{}, p);
   auto   b = psio::ssz::encode(p);
   REQUIRE(a == b);
}

TEST_CASE("ssz::decode scoped sugar matches psio::decode",
          "[ssz][format_tag_base]")
{
   auto buf  = psio::ssz::encode(std::string{"xyz"});
   auto back = psio::ssz::decode<std::string>(std::span<const char>{buf});
   REQUIRE(back == "xyz");
}
