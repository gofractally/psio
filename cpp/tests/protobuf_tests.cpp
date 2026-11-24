// Tests for psio/protobuf.hpp — schema-driven Protocol Buffers codec.
//
// Strategy: hand-craft byte sequences from the protobuf encoding
// spec (https://protobuf.dev/programming-guides/encoding/) for each
// scalar / container form, assert encode produces those exact bytes
// (proving wire-format conformance), and decode round-trips back to
// the original C++ value.  Compound types are checked for round-trip
// identity here; byte identity with the official implementation for
// every shape is protobuf_interop_tests.cpp.

#include <psio/protobuf.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "protobuf_random.hpp"
#include "protobuf_shapes.hpp"

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

TEST_CASE("protobuf: a zero scalar is not written (proto3 implicit presence)", "[protobuf]")
{
   test_pb::OneInt m{0};
   CHECK(psio::encode(psio::protobuf{}, m).empty());
   rt(m);
   // … and is read when a peer writes it.
   CHECK(psio::from_protobuf<test_pb::OneInt>(std::span<const char>{b({0x08, 0x00})}).value == 0);
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
   CHECK(psio::encode(psio::protobuf{}, test_pb::OneBool{false}).empty());
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

// ─── The mapping, beyond the first cut ──────────────────────────────

namespace
{
   template <typename T>
   bool accepts(const bytes_t& bytes)
   {
      const bool ok = psio::validate_protobuf<T>(std::span<const char>{bytes}).ok();
      // Validate and decode are one reader: the same verdict.
      T    out{};
      CHECK(psio::protobuf::decode_checked(std::span<const char>{bytes}, out).ok() == ok);
      return ok;
   }
   template <typename T>
   std::string refusal(const bytes_t& bytes)
   {
      const auto st = psio::validate_protobuf<T>(std::span<const char>{bytes});
      return st.ok() ? std::string{} : std::string{st.error().what};
   }
   template <typename T>
   T read(const bytes_t& bytes)
   {
      return psio::from_protobuf<T>(std::span<const char>{bytes});
   }
}  // namespace

struct PbClash
{
   std::variant<std::int32_t, std::string> v;
   std::int32_t                            after = 0;  // 2: taken by the variant
};
PSIO_REFLECT(PbClash, v, after)
struct PbZero
{
   std::int32_t a = 0;
};
PSIO_REFLECT(PbZero, attr(a, field<19000>))
static_assert(!psio::detail::pb_impl::slots_valid_v<PbClash>);
static_assert(!psio::detail::pb_impl::slots_valid_v<PbZero>);
static_assert(psio::detail::pb_impl::slots_valid_v<PbChoice>);

TEST_CASE("protobuf: zero values are absent, and absent is zero", "[protobuf]")
{
   CHECK(psio::to_protobuf(PbScalars{}).empty());
   CHECK(psio::to_protobuf(PbOrder{}).empty());
   CHECK(psio::to_protobuf(PbText{}).empty());
   // -0.0 is not the zero value.
   PbScalars n;
   n.d = -0.0;
   CHECK(psio::to_protobuf(n).size() == 9);
   // A member initialiser is not a default of the wire.
   const PbDefaults zero = read<PbDefaults>({});
   CHECK(zero.n == 0);
   CHECK(!zero.on);
   CHECK(zero.name.empty());
   CHECK(psio::to_protobuf(PbDefaults{}).size() > 10);
   CHECK(read<PbDefaults>(psio::to_protobuf(PbDefaults{})) == PbDefaults{});
}

TEST_CASE("protobuf: fields are written in number order", "[protobuf]")
{
   PbRenumbered v;
   v.a = 1, v.b = "b", v.c = 2, v.d = true, v.old = 9;
   CHECK(psio::to_protobuf(v) ==
         b({0x10, 9, 0x1a, 1, 'b', 0x38, 1, 0x80, 0x01, 1, 0xc0, 0x3e, 2}));
   CHECK(read<PbRenumbered>(b({0xc0, 0x3e, 2, 0x38, 1, 0x80, 0x01, 1, 0x1a, 1, 'b', 0x10, 9})) == v);
}

TEST_CASE("protobuf: std::optional is explicit presence", "[protobuf]")
{
   PbOptionals v;
   v.i = 0;
   v.s = "";
   CHECK(psio::to_protobuf(v) == b({0x08, 0, 0x1a, 0}));
   const PbOptionals back = read<PbOptionals>(b({0x08, 0, 0x1a, 0}));
   CHECK(back == v);
   CHECK(!back.b);
}

TEST_CASE("protobuf: std::variant is a oneof", "[protobuf]")
{
   PbChoice v;
   v.pick   = std::string{"x"};
   v.always = 2.5;
   // 6: string; 11: double (always set: a oneof alternative has presence)
   const auto bytes = psio::to_protobuf(v);
   CHECK(bytes == b({0x32, 1, 'x', 0x59, 0, 0, 0, 0, 0, 0, 4, 0x40}));
   CHECK(read<PbChoice>(bytes) == v);
   // Absent: std::monostate, or the first alternative's zero.
   const PbChoice none = read<PbChoice>({});
   CHECK(none.pick.index() == 0);
   CHECK(none.always == std::variant<bool, double>{false});
   CHECK(psio::to_protobuf(none) == b({0x50, 0}));
   // The last alternative on the wire wins.
   const PbChoice last = read<PbChoice>(b({0x32, 1, 'x', 0x28, 7}));
   CHECK(std::get<std::int32_t>(last.pick) == 7);
   // The same record alternative twice merges.
   const PbChoice merged = read<PbChoice>(b({0x3a, 2, 0x08, 1, 0x3a, 2, 0x10, 2}));
   CHECK(std::get<PbPoint>(merged.pick) == PbPoint{1, 2});
}

TEST_CASE("protobuf: std::map is a map", "[protobuf]")
{
   PbMaps v;
   v.counts = {{"b", 2}, {"a", 0}};
   // Entries in key order, key and value both written.
   const auto bytes = psio::to_protobuf(v);
   CHECK(bytes == b({0x0a, 5, 0x0a, 1, 'a', 0x10, 0, 0x0a, 5, 0x0a, 1, 'b', 0x10, 2}));
   CHECK(read<PbMaps>(bytes) == v);
   // Value before key, a missing key or value, an unknown field, the last
   // of a key.
   const PbMaps odd = read<PbMaps>(b({0x0a, 5, 0x10, 9, 0x0a, 1, 'k',  // value, key
                                      0x0a, 2, 0x10, 4,                // no key: ""
                                      0x0a, 3, 0x0a, 1, 'z',           // no value: 0
                                      0x0a, 7, 0x0a, 1, 'k', 0x18, 1, 0x10, 5}));  // k again
   CHECK(odd.counts == std::map<std::string, std::int32_t>{{"", 4}, {"k", 5}, {"z", 0}});
}

TEST_CASE("protobuf: repeated scalars are read packed or not", "[protobuf]")
{
   PbRepeated v;
   v.ints = {1, 2, 3};
   CHECK(psio::to_protobuf(v) == b({0x0a, 3, 1, 2, 3}));
   CHECK(read<PbRepeated>(b({0x08, 1, 0x08, 2, 0x08, 3})) == v);
   CHECK(read<PbRepeated>(b({0x08, 1, 0x0a, 2, 2, 3})) == v);
   CHECK(read<PbRepeated>(b({0x0a, 0, 0x0a, 1, 1, 0x0a, 2, 2, 3})) == v);
   PbRepeated f;
   f.floats = {1.0f};
   CHECK(read<PbRepeated>(b({0x25, 0, 0, 0x80, 0x3f})) == f);
   CHECK(refusal<PbRepeated>(b({0x22, 3, 0, 0, 0})) ==
         "protobuf: packed fixed values of a broken length");
}

TEST_CASE("protobuf: a singular field twice: the last, records merged", "[protobuf]")
{
   CHECK(read<PbPoint>(b({0x08, 1, 0x08, 2})).x == 2);
   PbOrder v = read<PbOrder>(b({0x12, 2, 0x08, 1, 0x12, 2, 0x10, 2}));
   CHECK(v.origin == PbPoint{1, 2});
}

TEST_CASE("protobuf: what a reader does not know is skipped", "[protobuf]")
{
   // Unknown numbers of every wire type, a group with a nested group, and
   // a known number with another wire type.
   const auto bytes = b({0x08, 5,                                  // x = 5
                         0x98, 0x06, 1,                            // 99: varint
                         0x99, 0x06, 1, 2, 3, 4, 5, 6, 7, 8,       // 99: fixed64
                         0x9a, 0x06, 2, 'h', 'i',                  // 99: bytes
                         0x9d, 0x06, 1, 2, 3, 4,                   // 99: fixed32
                         0x9b, 0x06, 0x08, 1, 0x13, 0x14, 0x9c, 0x06,  // 99: group { 1: 1, 2: group {} }
                         0x15, 1, 0, 0, 0});                       // y as fixed32: not y
   CHECK(accepts<PbPoint>(bytes));
   CHECK(read<PbPoint>(bytes) == PbPoint{5, 0});
   CHECK(refusal<PbPoint>(b({0x9b, 0x06, 0x08, 1})) == "protobuf: unterminated group");
   CHECK(refusal<PbPoint>(b({0x9b, 0x06, 0x94, 0x06})) ==
         "protobuf: a group ends with another field number");
   CHECK(refusal<PbPoint>(b({0x0c})) == "protobuf: end of a group that was not begun");
}

TEST_CASE("protobuf: bytes that are not a message", "[protobuf]")
{
   CHECK(refusal<PbPoint>(b({0x08})) == "protobuf: truncated varint");
   CHECK(refusal<PbPoint>(b({0x00, 0x00})) == "protobuf: field number 0");
   CHECK(refusal<PbPoint>(b({0x0e})) == "protobuf: unknown wire type");
   CHECK(refusal<PbPoint>(b({0x0f})) == "protobuf: unknown wire type");
   CHECK(refusal<PbPoint>(b({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02})) ==
         "protobuf: varint exceeds 64 bits");
   // -1 as an int32: ten bytes.
   CHECK(read<PbPoint>(b({0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01})).x == -1);
   // A 64-bit varint in an int32 is truncated (as every implementation does).
   CHECK(read<PbPoint>(b({0x08, 0x81, 0x80, 0x80, 0x80, 0x10})).x == 1);
   CHECK(refusal<PbText>(b({0x0a, 2, 0xc3, 0x28})) == "protobuf: a string is not UTF-8");
   CHECK(refusal<PbText>(b({0x0a, 3, 0xed, 0xa0, 0x80})) == "protobuf: a string is not UTF-8");
   CHECK(accepts<PbText>(b({0x12, 2, 0xc3, 0x28})));  // bytes hold anything
   // A length the input does not have: refused before anything is made.
   CHECK(refusal<PbText>(b({0x12, 0xff, 0xff, 0xff, 0xff, 0x07, 1, 2})) ==
         "protobuf: length exceeds the input");
   CHECK(refusal<PbRepeated>(b({0x0a, 0xf0, 0xff, 0xff, 0x7f, 1})) ==
         "protobuf: length exceeds the input");
   CHECK(refusal<PbText>(b({0x22, 3, 1, 2, 3})) ==
         "protobuf: bytes of a fixed size have another length");
   // What a narrow C++ type cannot hold.
   CHECK(refusal<PbScalars>(b({0x10, 0x80, 0x01})) == "protobuf: a value does not fit its type");
   CHECK(read<PbScalars>(b({0x10, 0x7f})).i8 == 127);
   CHECK(refusal<PbScalars>(b({0x68, 0x80, 0x02})) == "protobuf: a value does not fit its type");
   // An open enum: a value without a name is kept.
   CHECK(read<PbScalars>(b({0x68, 0x4d})).color == static_cast<PbColor>(77));
   CHECK(read<PbScalars>(b({0x70, 0xc0, 0xc4, 0x07})).level == static_cast<PbLevel>(123456));
}

TEST_CASE("protobuf: nesting is bounded", "[protobuf]")
{
   const auto nested = [](unsigned depth) {
      bytes_t out;
      for (unsigned i = 0; i < depth; ++i)
      {
         bytes_t     outer{0x12};
         std::size_t n = out.size();
         for (; n >= 0x80; n >>= 7)
            outer.push_back(static_cast<char>(n | 0x80));
         outer.push_back(static_cast<char>(n));
         outer.insert(outer.end(), out.begin(), out.end());
         out = std::move(outer);
      }
      return out;
   };
   CHECK(accepts<PbTree>(nested(100)));
   CHECK(refusal<PbTree>(nested(101)) == "protobuf: nested too deeply");
   const auto deep = nested(40);
   CHECK(psio::validate_protobuf<PbTree>(std::span<const char>{deep}, {.max_depth = 40}).ok());
   CHECK(!psio::validate_protobuf<PbTree>(std::span<const char>{deep}, {.max_depth = 39}).ok());
   CHECK(psio::from_protobuf<PbTree>(std::span<const char>{deep}, {.max_depth = 64}).children.size() == 1);
   // Groups nest too.
   bytes_t groups;
   for (int i = 0; i < 200; ++i)
      groups.push_back(0x1b);
   for (int i = 0; i < 200; ++i)
      groups.push_back(0x1c);
   CHECK(refusal<PbPoint>(groups) == "protobuf: nested too deeply");
}

TEST_CASE("protobuf: every shape round-trips", "[protobuf]")
{
   pb_rng r{77};
   for (int i = 0; i < 500; ++i)
   {
      const PbOrder v     = pb_random<PbOrder>(r);
      const auto    bytes = psio::to_protobuf(v);
      REQUIRE(psio::protobuf_size(v) == bytes.size());
      REQUIRE(psio::validate_protobuf<PbOrder>(std::span<const char>{bytes}).ok());
      REQUIRE(read<PbOrder>(bytes) == v);
      const PbEncodings e = pb_random<PbEncodings>(r);
      REQUIRE(read<PbEncodings>(psio::to_protobuf(e)) == e);
      const PbOptionals o = pb_random<PbOptionals>(r);
      REQUIRE(read<PbOptionals>(psio::to_protobuf(o)) == o);
      // Appending to a sink keeps what is there.
      std::vector<char> sink{'x'};
      psio::to_protobuf(o, sink);
      REQUIRE(sink.size() == 1 + psio::protobuf_size(o));
      REQUIRE(sink.front() == 'x');
   }
}
