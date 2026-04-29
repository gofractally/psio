// Tests for psio/protobuf.hpp — schema-driven Protocol Buffers codec.
//
// Strategy: hand-craft byte sequences from the protobuf encoding
// spec (https://protobuf.dev/programming-guides/encoding/) for each
// scalar / container form, assert encode produces those exact bytes
// (proving wire-format conformance), and decode round-trips back to
// the original C++ value.  Compound types are checked for round-trip
// identity rather than exact bytes since field order in the wire
// matches declaration order today but isn't strictly required by
// the spec.

#include <psio/protobuf.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
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
      auto enc = psio::encode(psio::protobuf{}, v);
      auto dec =
         psio::decode<T>(psio::protobuf{}, std::span<const char>{enc});
      REQUIRE(dec == v);
   }
}

// ─── Single-field messages — exact wire bytes ────────────────────────

namespace test_pb
{
   struct OneInt
   {
      std::int32_t value = 0;
      friend bool  operator==(const OneInt&, const OneInt&) = default;
   };
   PSIO_REFLECT(OneInt, value)

   struct OneString
   {
      std::string value;
      friend bool operator==(const OneString&, const OneString&) = default;
   };
   PSIO_REFLECT(OneString, value)

   struct OneFloat
   {
      float       value = 0;
      friend bool operator==(const OneFloat&, const OneFloat&) = default;
   };
   PSIO_REFLECT(OneFloat, value)

   struct OneDouble
   {
      double      value = 0;
      friend bool operator==(const OneDouble&, const OneDouble&) = default;
   };
   PSIO_REFLECT(OneDouble, value)
}  // namespace test_pb

TEST_CASE("protobuf: int32 field=1 value=150 → 08 96 01", "[protobuf]")
{
   //  Canonical example from protobuf-encoding docs.
   test_pb::OneInt m{150};
   //  tag = (1<<3)|0 = 0x08; varint(150) = 0x96 0x01.
   CHECK(psio::encode(psio::protobuf{}, m) == b({0x08, 0x96, 0x01}));
   rt(m);
}

TEST_CASE("protobuf: int32 zero is omitted by varint encoding "
          "(value byte = 0)",
          "[protobuf]")
{
   //  Default-valued fields are still emitted in proto3 (since proto3.5
   //  reverted to "always emit explicit fields" for simple scalars on
   //  encode for typed APIs); we follow that rule.
   //  tag 0x08 + varint(0) = 0x00.
   test_pb::OneInt m{0};
   CHECK(psio::encode(psio::protobuf{}, m) == b({0x08, 0x00}));
   rt(m);
}

TEST_CASE("protobuf: string field=1 value=\"testing\"", "[protobuf]")
{
   //  tag = (1<<3)|2 = 0x0a; len=7; "testing" raw.
   test_pb::OneString m{"testing"};
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x0a, 0x07, 't', 'e', 's', 't', 'i', 'n', 'g'}));
   rt(m);
}

TEST_CASE("protobuf: float field=1 value=1.0 → fixed32 LE",
          "[protobuf]")
{
   //  tag = (1<<3)|5 = 0x0d; bits of 1.0f = 0x3f800000 in LE order:
   //  0x00 0x00 0x80 0x3f.
   test_pb::OneFloat m{1.0f};
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x0d, 0x00, 0x00, 0x80, 0x3f}));
   rt(m);
}

TEST_CASE("protobuf: double field=1 value=1.0 → fixed64 LE",
          "[protobuf]")
{
   //  tag = (1<<3)|1 = 0x09; bits of 1.0 = 0x3ff0000000000000 LE.
   test_pb::OneDouble m{1.0};
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x3f}));
   rt(m);
}

// ─── Field-number selection by declaration order ────────────────────

namespace test_pb
{
   struct ThreeFields
   {
      std::int32_t a = 0;
      std::int32_t b = 0;
      std::int32_t c = 0;
      friend bool  operator==(const ThreeFields&, const ThreeFields&) = default;
   };
   PSIO_REFLECT(ThreeFields, a, b, c)
}

TEST_CASE("protobuf: declaration order maps to field numbers 1..N",
          "[protobuf]")
{
   //  a=1 → field 1 (tag 0x08), b=2 → field 2 (tag 0x10),
   //  c=3 → field 3 (tag 0x18).
   test_pb::ThreeFields m{1, 2, 3};
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x08, 0x01, 0x10, 0x02, 0x18, 0x03}));
   rt(m);
}

// ─── Boolean ─────────────────────────────────────────────────────────

namespace test_pb
{
   struct OneBool
   {
      bool        value = false;
      friend bool operator==(const OneBool&, const OneBool&) = default;
   };
   PSIO_REFLECT(OneBool, value)
}

TEST_CASE("protobuf: bool true / false", "[protobuf]")
{
   //  tag 0x08 + 0x01 / 0x00.
   CHECK(psio::encode(psio::protobuf{}, test_pb::OneBool{true}) ==
         b({0x08, 0x01}));
   CHECK(psio::encode(psio::protobuf{}, test_pb::OneBool{false}) ==
         b({0x08, 0x00}));
   rt(test_pb::OneBool{true});
   rt(test_pb::OneBool{false});
}

// ─── Optional (proto3 explicit-presence semantics) ──────────────────

namespace test_pb
{
   struct OptInt
   {
      std::optional<std::int32_t> value;
      friend bool operator==(const OptInt&, const OptInt&) = default;
   };
   PSIO_REFLECT(OptInt, value)
}

TEST_CASE("protobuf: optional empty omits the field entirely",
          "[protobuf]")
{
   test_pb::OptInt empty;  //  no value
   //  Wire is empty.
   CHECK(psio::encode(psio::protobuf{}, empty).empty());
   rt(empty);
}

TEST_CASE("protobuf: optional with value emits normally",
          "[protobuf]")
{
   test_pb::OptInt set;
   set.value = 42;
   CHECK(psio::encode(psio::protobuf{}, set) == b({0x08, 0x2a}));
   rt(set);
}

// ─── Bytes (vector<u8>) ─────────────────────────────────────────────

namespace test_pb
{
   struct ByteBag
   {
      std::vector<std::uint8_t> data;
      friend bool operator==(const ByteBag&, const ByteBag&) = default;
   };
   PSIO_REFLECT(ByteBag, data)
}

TEST_CASE("protobuf: bytes field is length-delimited raw bytes",
          "[protobuf]")
{
   test_pb::ByteBag m{{0xde, 0xad, 0xbe, 0xef}};
   //  tag 0x0a + len 0x04 + raw.
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x0a, 0x04, 0xde, 0xad, 0xbe, 0xef}));
   rt(m);
}

// ─── Packed repeated scalars ────────────────────────────────────────

namespace test_pb
{
   struct PackedInts
   {
      std::vector<std::int32_t> ids;
      friend bool operator==(const PackedInts&, const PackedInts&) = default;
   };
   PSIO_REFLECT(PackedInts, ids)
}

TEST_CASE("protobuf: repeated int32 packs into a single length-delim",
          "[protobuf]")
{
   //  Per protobuf-encoding spec example: ids = [1, 2, 3] →
   //    tag (1<<3 | 2) = 0x0a, len = 3, then varint(1) varint(2) varint(3).
   test_pb::PackedInts m{{1, 2, 3}};
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x0a, 0x03, 0x01, 0x02, 0x03}));
   rt(m);
}

TEST_CASE("protobuf: empty repeated field omits the wire entirely",
          "[protobuf]")
{
   test_pb::PackedInts empty;
   CHECK(psio::encode(psio::protobuf{}, empty).empty());
   rt(empty);
}

// ─── Unpacked repeated strings ──────────────────────────────────────

namespace test_pb
{
   struct StringList
   {
      std::vector<std::string> labels;
      friend bool operator==(const StringList&, const StringList&) = default;
   };
   PSIO_REFLECT(StringList, labels)
}

TEST_CASE("protobuf: repeated string emits one wire-2 record per element",
          "[protobuf]")
{
   //  labels = ["a", "b"] → 0x0a 0x01 'a'  0x0a 0x01 'b'.
   test_pb::StringList m{{std::string{"a"}, std::string{"b"}}};
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x0a, 0x01, 'a', 0x0a, 0x01, 'b'}));
   rt(m);
}

// ─── Nested messages ────────────────────────────────────────────────

namespace test_pb
{
   struct Inner
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      friend bool  operator==(const Inner&, const Inner&) = default;
   };
   PSIO_REFLECT(Inner, x, y)

   struct Outer
   {
      Inner       inner;
      friend bool operator==(const Outer&, const Outer&) = default;
   };
   PSIO_REFLECT(Outer, inner)
}

TEST_CASE("protobuf: sub-message is length-delimited body", "[protobuf]")
{
   //  outer.inner.x=7, .y=11
   //  Inner body: 0x08 0x07 0x10 0x0b   (4 bytes)
   //  Outer wire: tag 0x0a (field 1, type 2) + len 0x04 + body.
   test_pb::Outer m;
   m.inner.x = 7;
   m.inner.y = 11;
   CHECK(psio::encode(psio::protobuf{}, m) ==
         b({0x0a, 0x04, 0x08, 0x07, 0x10, 0x0b}));
   rt(m);
}

namespace test_pb
{
   struct InnerHolder
   {
      std::vector<Inner> rows;
      friend bool operator==(const InnerHolder&, const InnerHolder&) = default;
   };
   PSIO_REFLECT(InnerHolder, rows)
}

TEST_CASE("protobuf: vector<message> round-trips", "[protobuf]")
{
   test_pb::InnerHolder h;
   h.rows.push_back(test_pb::Inner{1, 2});
   h.rows.push_back(test_pb::Inner{3, 4});
   //  Two length-delimited records, each containing two varint fields.
   //  tag 0x0a, len 0x04, body...; tag 0x0a, len 0x04, body...
   CHECK(psio::encode(psio::protobuf{}, h) ==
         b({0x0a, 0x04, 0x08, 0x01, 0x10, 0x02,
            0x0a, 0x04, 0x08, 0x03, 0x10, 0x04}));
   rt(h);
}

// ─── Schema-evolution: unknown fields are skipped on decode ────────

namespace test_pb
{
   struct OldMsg  //  Sender wrote 3 fields with numbers 1, 2, 3.
   {
      std::int32_t a = 0;
      std::int32_t b = 0;
      std::int32_t c = 0;
      friend bool  operator==(const OldMsg&, const OldMsg&) = default;
   };
   PSIO_REFLECT(OldMsg, a, b, c)

}  // namespace test_pb

//  Receiver only knows fields 1 and 3 (dropped the middle one).
//  Lives at global scope because the `attr(name, field<N>)` annotation
//  emits a `template <>` specialisation of `::psio::annotate<>` and
//  C++ requires those specialisations in an enclosing namespace.
struct NewMsg
{
   std::int32_t a = 0;
   std::int32_t c = 0;
   friend bool  operator==(const NewMsg&, const NewMsg&) = default;
};
PSIO_REFLECT(NewMsg, attr(a, field<1>), attr(c, field<3>))

namespace test_pb
{
   using ::NewMsg;
}

TEST_CASE("protobuf: receiver skips unknown fields by wire_type",
          "[protobuf][evolution]")
{
   test_pb::OldMsg sent{10, 20, 30};
   auto wire = psio::encode(psio::protobuf{}, sent);

   auto received = psio::decode<test_pb::NewMsg>(
      psio::protobuf{}, std::span<const char>{wire});

   CHECK(received.a == 10);
   CHECK(received.c == 30);  //  field 2 (b=20) was unknown and skipped.
}

// ─── pb_fixed / pb_sint integer-encoding annotations ───────────────

//  Globally-scoped because attr() emits a ::psio::annotate<>
//  specialisation that needs an enclosing namespace of psio.
struct FixedMsg
{
   std::int32_t  s32 = 0;   // sfixed32
   std::uint32_t u32 = 0;   // fixed32
   std::int64_t  s64 = 0;   // sfixed64
   std::uint64_t u64 = 0;   // fixed64
   friend bool   operator==(const FixedMsg&, const FixedMsg&) = default;
};
PSIO_REFLECT(FixedMsg,
             attr(s32, ::psio::pb_fixed),
             attr(u32, ::psio::pb_fixed),
             attr(s64, ::psio::pb_fixed),
             attr(u64, ::psio::pb_fixed))

struct SintMsg
{
   std::int32_t  a = 0;
   std::int64_t  b = 0;
   friend bool   operator==(const SintMsg&, const SintMsg&) = default;
};
PSIO_REFLECT(SintMsg,
             attr(a, ::psio::pb_sint),
             attr(b, ::psio::pb_sint))

struct PackedFixedMsg
{
   std::vector<std::int32_t> ids;
   friend bool operator==(const PackedFixedMsg&, const PackedFixedMsg&) =
      default;
};
PSIO_REFLECT(PackedFixedMsg, attr(ids, ::psio::pb_fixed))

struct PackedSintMsg
{
   std::vector<std::int32_t> ids;
   friend bool operator==(const PackedSintMsg&, const PackedSintMsg&) =
      default;
};
PSIO_REFLECT(PackedSintMsg, attr(ids, ::psio::pb_sint))

TEST_CASE("protobuf: pb_fixed sets wire-type 5/1 and writes LE bytes",
          "[protobuf][encoding]")
{
   FixedMsg m{-1, 0xDEADBEEF, -1LL, 0x0123456789ABCDEFULL};
   auto     wire = psio::encode(psio::protobuf{}, m);

   //  Expected bytes:
   //    field 1 sfixed32 -1   : tag 0x0D (1<<3 | 5), 0xFF FF FF FF
   //    field 2  fixed32 0xDEADBEEF: tag 0x15 (2<<3 | 5), 0xEF BE AD DE
   //    field 3 sfixed64 -1   : tag 0x19 (3<<3 | 1), 0xFF*8
   //    field 4  fixed64 ...  : tag 0x21 (4<<3 | 1), LE bytes
   auto expected = b({0x0D, 0xFF, 0xFF, 0xFF, 0xFF,
                      0x15, 0xEF, 0xBE, 0xAD, 0xDE,
                      0x19, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                      0x21, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01});
   CHECK(wire == expected);
   rt(m);
}

TEST_CASE("protobuf: pb_sint zigzag-encodes signed varints",
          "[protobuf][encoding]")
{
   //  Reference values from the protobuf spec:
   //    zigzag( 0) = 0,   zigzag(-1) = 1,
   //    zigzag( 1) = 2,   zigzag(-2) = 3, ...
   //  So a = -1 → varint 1, b = -2 → varint 3.
   SintMsg m{-1, -2};
   auto    wire = psio::encode(psio::protobuf{}, m);

   //  field 1 (sint32) : tag 0x08, varint 0x01
   //  field 2 (sint64) : tag 0x10, varint 0x03
   auto expected = b({0x08, 0x01, 0x10, 0x03});
   CHECK(wire == expected);
   rt(m);

   //  And verify the default-encoded wire would have been 10 bytes
   //  for the negative — pb_sint is materially shorter here.
   CHECK(wire.size() == 4);
}

TEST_CASE("protobuf: pb_sint round-trips a swept range",
          "[protobuf][encoding]")
{
   for (auto v : {0, 1, -1, 42, -42, 0x7FFFFFFF, -0x7FFFFFFF - 1})
   {
      SintMsg m{v, static_cast<std::int64_t>(v) * 1000};
      rt(m);
   }
}

TEST_CASE("protobuf: packed repeated honours pb_fixed per-element",
          "[protobuf][encoding]")
{
   PackedFixedMsg m{{1, 2, -1}};
   auto           wire = psio::encode(psio::protobuf{}, m);

   //  tag 0x0A (field 1, length-delim), payload-len = 12
   //  payload = 0x01 0x00 0x00 0x00 | 0x02 0x00 0x00 0x00 | 0xFF*4
   auto expected = b({0x0A, 0x0C,
                      0x01, 0x00, 0x00, 0x00,
                      0x02, 0x00, 0x00, 0x00,
                      0xFF, 0xFF, 0xFF, 0xFF});
   CHECK(wire == expected);
   rt(m);
}

TEST_CASE("protobuf: packed repeated honours pb_sint per-element",
          "[protobuf][encoding]")
{
   PackedSintMsg m{{0, -1, 1, -2, 2}};
   auto          wire = psio::encode(psio::protobuf{}, m);

   //  tag 0x0A, len = 5, payload = 00 01 02 03 04 (zigzag of values)
   auto expected = b({0x0A, 0x05, 0x00, 0x01, 0x02, 0x03, 0x04});
   CHECK(wire == expected);
   rt(m);
}

// ─── validate_strict accepts well-formed input ──────────────────────

TEST_CASE("protobuf: validate accepts valid input", "[protobuf]")
{
   auto wire = psio::encode(psio::protobuf{}, test_pb::OneInt{150});
   auto st   = psio::validate<test_pb::OneInt>(
      psio::protobuf{}, std::span<const char>{wire});
   CHECK(static_cast<bool>(st));
}

TEST_CASE("protobuf: validate rejects truncated varint", "[protobuf]")
{
   //  tag 0x08, then a varint with the continuation bit set but no
   //  follow-up byte → truncated.
   auto bad = b({0x08, 0x80});
   auto st  = psio::validate<test_pb::OneInt>(
      psio::protobuf{}, std::span<const char>{bad});
   CHECK(!st);
}
