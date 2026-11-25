// What a type declares about its values is enforced by the validating read
// paths of the formats whose bytes follow another specification:
// FlatBuffers, Cap'n Proto, protobuf (psio/value_contract.hpp;
// gofractally/psio#14). A value outside a member's bound has the same
// verdict in every format.
//
// Each buffer is written from a twin type — the same members without the
// declarations, what a writer that does not know them produces — and read as
// the declared type.

#include <psio/capnp.hpp>
#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/value_contract.hpp>
#include <psio/view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "flatbuf_shapes.hpp"
#include "protobuf_shapes.hpp"

// ── One member, one declaration ─────────────────────────────────────────────
#define VC_PAIR(NAME, MEMBER, ANN)                                \
   struct NAME##Loose                                             \
   {                                                              \
      MEMBER;                                                     \
      PSIO_REFLECT(NAME##Loose, v)                                \
   };                                                             \
   struct NAME                                                    \
   {                                                              \
      MEMBER;                                                     \
      PSIO_REFLECT(NAME, attr(v, ANN))                            \
   };

using VcInts    = std::vector<std::uint32_t>;
using VcStrings = std::vector<std::string>;
using VcMap     = std::map<std::string, std::string>;

VC_PAIR(VcMaxList, VcInts v, max<2>)
VC_PAIR(VcMaxString, std::string v, max<2>)
VC_PAIR(VcMaxBytes, std::vector<std::uint8_t> v, max<2>)
VC_PAIR(VcMinList, VcInts v, min<2>)
VC_PAIR(VcExactList, VcInts v, exact<2>)
VC_PAIR(VcRange, std::string v, min<2> | max<4>)
VC_PAIR(VcElement, VcStrings v, element(max<2>))
VC_PAIR(VcUtf8, std::string v, utf8)
VC_PAIR(VcSorted, VcInts v, sorted)
VC_PAIR(VcUnique, VcInts v, unique)
VC_PAIR(VcOptional, std::optional<std::string> v, max<2>)
VC_PAIR(VcMaxMap, VcMap v, max<1>)
VC_PAIR(VcKeys, VcMap v, keys(max<1>))
VC_PAIR(VcValues, VcMap v, values(max<1>))

struct VcEvenLoose
{
   std::uint32_t v = 0;
   PSIO_REFLECT(VcEvenLoose, v)
};
struct VcEven
{
   std::uint32_t             v = 0;
   static psio::codec_status even(const VcEven& e) noexcept
   {
      return e.v % 2 == 0 ? psio::codec_ok() : psio::codec_fail("VcEven: v is odd", 0, "VcEven");
   }
   PSIO_REFLECT(VcEven, v, invariant(even))
};

// A declaration below a record that declares nothing itself.
struct VcOuterLoose
{
   std::vector<VcMaxListLoose> rows;
   std::optional<VcEvenLoose>  even;
   PSIO_REFLECT(VcOuterLoose, rows, even)
};
struct VcOuter
{
   std::vector<VcMaxList> rows;
   std::optional<VcEven>  even;
   PSIO_REFLECT(VcOuter, rows, even)
};

// ── has_value_contract_v ────────────────────────────────────────────────────
static_assert(psio::has_value_contract_v<VcMaxList>);
static_assert(psio::has_value_contract_v<VcEven>);
static_assert(psio::has_value_contract_v<VcOuter>);
static_assert(psio::has_value_contract_v<Bounded>);
static_assert(psio::has_value_contract_v<PbBounded>);
static_assert(psio::has_value_contract_v<std::vector<std::optional<VcOuter>>>);
// A type that declares nothing has nothing to check: its validators do what
// they did before, and accept what they accepted.
static_assert(!psio::has_value_contract_v<VcMaxListLoose>);
static_assert(!psio::has_value_contract_v<VcOuterLoose>);
static_assert(!psio::has_value_contract_v<Loose>);
static_assert(!psio::has_value_contract_v<Profile>);
static_assert(!psio::has_value_contract_v<Order>);
static_assert(!psio::has_value_contract_v<Tree>);     // holds itself
static_assert(!psio::has_value_contract_v<Basket>);
static_assert(!psio::has_value_contract_v<PbOrder>);
static_assert(!psio::has_value_contract_v<PbTree>);   // holds itself
static_assert(!psio::has_value_contract_v<PbLoose>);

namespace
{
   using Bytes = std::vector<char>;

   template <typename Fmt>
   constexpr std::string_view length_message()
   {
      if constexpr (std::is_same_v<Fmt, psio::flatbuf>)
         return "flatbuf: member length_bound violated";
      else if constexpr (std::is_same_v<Fmt, psio::capnp>)
         return "capnp: member length_bound violated";
      else
         return "protobuf: member length_bound violated";
   }

   // `bad` (a Loose value outside what T declares) and `good` (inside) as
   // format Fmt: every validating read path refuses the first with `error`
   // and accepts the second.
   template <typename Fmt, typename T, typename L>
   void expect(const L& bad, const L& good, std::string_view error)
   {
      const Bytes                 bad_bytes  = psio::encode(Fmt{}, bad);
      const Bytes                 good_bytes = psio::encode(Fmt{}, good);
      const std::span<const char> b{bad_bytes}, g{good_bytes};

      // The loose type accepts both.
      REQUIRE(psio::validate<L>(Fmt{}, b).ok());
      REQUIRE(psio::validate<L>(Fmt{}, g).ok());

      // validate
      const auto st = psio::validate<T>(Fmt{}, b);
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == error);
      CHECK(st.error().byte_offset == 0);
      CHECK(psio::validate<T>(Fmt{}, g).ok());

      // validate_strict
      CHECK_FALSE(psio::validate_strict<T>(Fmt{}, b).ok());
      CHECK(psio::validate_strict<T>(Fmt{}, g).ok());

      // The checked decode: from_F<T>(bytes).
      if constexpr (std::is_same_v<Fmt, psio::flatbuf>)
      {
         CHECK_THROWS_AS(psio::from_flatbuf<T>(b), psio::codec_exception);
         CHECK_NOTHROW(psio::from_flatbuf<T>(g));
         CHECK_FALSE(psio::validate_flatbuf<T>(b).ok());
         CHECK_FALSE(psio::validate_flatbuf<T>(b, psio::flatbuf::limits{}).ok());
         CHECK_FALSE(psio::validate_flatbuf<T>(b, psio::flatbuf::options{}).ok());
         // The validating view.
         CHECK_FALSE(psio::validated_view<T>(psio::flatbuf{}, b).ok());
         CHECK(psio::validated_view<T>(psio::flatbuf{}, g).ok());
      }
      else if constexpr (std::is_same_v<Fmt, psio::capnp>)
      {
         CHECK_THROWS_AS(psio::from_capnp<T>(b), psio::codec_exception);
         CHECK_NOTHROW(psio::from_capnp<T>(g));
         CHECK_FALSE(psio::validate_capnp<T>(b).ok());
         CHECK_FALSE(psio::validate_capnp<T>(b, psio::capnp::limits{}).ok());
         // (A capnp view is taken over a message `validate_capnp` accepted;
         // there is no other validating entry point.)
      }
      else
      {
         CHECK_THROWS_AS(psio::from_protobuf<T>(b), psio::codec_exception);
         CHECK_NOTHROW(psio::from_protobuf<T>(g));
         CHECK_THROWS_AS(psio::from_protobuf<T>(b, psio::protobuf::limits{}), psio::codec_exception);
         CHECK_FALSE(psio::validate_protobuf<T>(b).ok());
         CHECK_FALSE(psio::validate_protobuf<T>(b, psio::protobuf::limits{}).ok());
         T out{};
         CHECK_FALSE(psio::protobuf::decode_checked(b, out).ok());
         CHECK(psio::protobuf::decode_checked(g, out).ok());
      }
   }

   template <typename T, typename L, typename V>
   L holding(V v)
   {
      L l;
      l.v = std::move(v);
      return l;
   }

   template <typename Fmt>
   void lists_and_strings()
   {
      const auto length = length_message<Fmt>();
      expect<Fmt, VcMaxList>(VcMaxListLoose{{1, 2, 3}}, VcMaxListLoose{{1, 2}}, length);
      expect<Fmt, VcMaxString>(VcMaxStringLoose{"abc"}, VcMaxStringLoose{"ab"}, length);
      expect<Fmt, VcMaxBytes>(VcMaxBytesLoose{{1, 2, 3}}, VcMaxBytesLoose{{1, 2}}, length);
      expect<Fmt, VcMinList>(VcMinListLoose{{1}}, VcMinListLoose{{1, 2}}, length);
      // An absent member is its empty value, and that is under min<2>.
      expect<Fmt, VcMinList>(VcMinListLoose{}, VcMinListLoose{{1, 2, 3}}, length);
      expect<Fmt, VcExactList>(VcExactListLoose{{1, 2, 3}}, VcExactListLoose{{1, 2}}, length);
      expect<Fmt, VcExactList>(VcExactListLoose{{1}}, VcExactListLoose{{4, 5}}, length);
      expect<Fmt, VcRange>(VcRangeLoose{"a"}, VcRangeLoose{"ab"}, length);
      expect<Fmt, VcRange>(VcRangeLoose{"abcde"}, VcRangeLoose{"abcd"}, length);
      expect<Fmt, VcElement>(VcElementLoose{{"ab", "abc"}}, VcElementLoose{{"ab", ""}}, length);
      expect<Fmt, VcSorted>(VcSortedLoose{{2, 1}}, VcSortedLoose{{1, 1, 2}},
                            "sorted: elements are not in order");
      expect<Fmt, VcUnique>(VcUniqueLoose{{1, 1}}, VcUniqueLoose{{1, 2}},
                            "sorted: elements are not in strictly ascending order");
      expect<Fmt, VcOptional>(VcOptionalLoose{std::string{"abc"}}, VcOptionalLoose{std::string{"ab"}},
                              length);
      expect<Fmt, VcOptional>(VcOptionalLoose{std::string{"abc"}}, VcOptionalLoose{}, length);
      expect<Fmt, VcEven>(VcEvenLoose{3}, VcEvenLoose{4}, "VcEven: v is odd");
      // Below a record that declares nothing itself.
      expect<Fmt, VcOuter>(VcOuterLoose{{{{1}}, {{1, 2, 3}}}, {}}, VcOuterLoose{{{{1}}, {{1, 2}}}, {}},
                           length);
      expect<Fmt, VcOuter>(VcOuterLoose{{}, VcEvenLoose{5}}, VcOuterLoose{{}, VcEvenLoose{6}},
                           "VcEven: v is odd");
   }

   template <typename Fmt>
   void maps()
   {
      const auto length = length_message<Fmt>();
      expect<Fmt, VcMaxMap>(VcMaxMapLoose{{{"a", "b"}, {"c", "d"}}}, VcMaxMapLoose{{{"a", "b"}}},
                            length);
      expect<Fmt, VcKeys>(VcKeysLoose{{{"ab", "b"}}}, VcKeysLoose{{{"a", "bbb"}}}, length);
      expect<Fmt, VcValues>(VcValuesLoose{{{"a", "bc"}}}, VcValuesLoose{{{"aaa", "b"}}}, length);
   }
}  // namespace

TEST_CASE("value contract: flatbuf refuses a value outside what its type declares",
          "[value_contract][flatbuf]")
{
   lists_and_strings<psio::flatbuf>();
   maps<psio::flatbuf>();
   // A FlatBuffers string is bytes; a member declared utf8 is text.
   expect<psio::flatbuf, VcUtf8>(VcUtf8Loose{"\xff\xfe"}, VcUtf8Loose{"\xc3\xa9"},
                                 "utf8_spec: invalid leading byte");
}

TEST_CASE("value contract: capnp refuses a value outside what its type declares",
          "[value_contract][capnp]")
{
   lists_and_strings<psio::capnp>();
   expect<psio::capnp, VcUtf8>(VcUtf8Loose{"\xff\xfe"}, VcUtf8Loose{"\xc3\xa9"},
                               "utf8_spec: invalid leading byte");
}

TEST_CASE("value contract: protobuf refuses a value outside what its type declares",
          "[value_contract][protobuf]")
{
   lists_and_strings<psio::protobuf>();
   maps<psio::protobuf>();
}

TEST_CASE("value contract: protobuf holds the declarations against the finished value",
          "[value_contract][protobuf]")
{
   // `v` (field 1) written twice: the last string is the value.
   const Bytes replaced{0x0a, 3, 'a', 'b', 'c', 0x0a, 2, 'o', 'k'};
   CHECK(psio::validate_protobuf<VcMaxString>(replaced).ok());
   CHECK(psio::from_protobuf<VcMaxString>(replaced).v == "ok");
   const Bytes replaced_bad{0x0a, 2, 'o', 'k', 0x0a, 3, 'a', 'b', 'c'};
   CHECK_FALSE(psio::validate_protobuf<VcMaxString>(replaced_bad).ok());
   // A packed list written in two parts is one list.
   const Bytes two_parts{0x0a, 2, 1, 2, 0x0a, 1, 3};
   CHECK(psio::validate_protobuf<VcMaxListLoose>(two_parts).ok());
   CHECK_FALSE(psio::validate_protobuf<VcMaxList>(two_parts).ok());
}

TEST_CASE("value contract: the structure of the bytes is checked first",
          "[value_contract]")
{
   Bytes fb = psio::to_flatbuf(VcMaxListLoose{{1, 2, 3}});
   fb.resize(fb.size() - 2);
   const auto st = psio::validate_flatbuf<VcMaxList>(fb);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what != std::string_view{"flatbuf: member length_bound violated"});
   CHECK(st.error().what == psio::validate_flatbuf<VcMaxListLoose>(fb).error().what);

   Bytes pb = psio::to_protobuf(VcMaxListLoose{{1, 2, 3}});
   pb.pop_back();
   const auto pst = psio::validate_protobuf<VcMaxList>(pb);
   REQUIRE_FALSE(pst.ok());
   CHECK(pst.error().what == psio::validate_protobuf<VcMaxListLoose>(pb).error().what);
   CHECK(pst.error().byte_offset == psio::validate_protobuf<VcMaxListLoose>(pb).error().byte_offset);
}

TEST_CASE("value contract: the size-prefixed and identified FlatBuffers forms",
          "[value_contract][flatbuf]")
{
   const psio::flatbuf::options opts{.file_identifier = "BNDD", .size_prefixed = true};
   Loose                        over = conformance_loose();
   CHECK(psio::validate_flatbuf<Bounded>(psio::to_flatbuf(over, opts), opts).ok());
   over.ids.push_back(11);
   const auto st = psio::validate_flatbuf<Bounded>(psio::to_flatbuf(over, opts), opts);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == std::string_view{"flatbuf: member length_bound violated"});
}

TEST_CASE("value contract: the verdict is the one of pssz, key and json",
          "[value_contract]")
{
   // The same violation, declared once, in six formats.
   const VcMaxList over{{1, 2, 3}};
   CHECK_THROWS_AS(psio::to_pssz(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_key(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_json(over), psio::codec_exception);
   const VcMaxListLoose loose{{1, 2, 3}};
   CHECK_FALSE(psio::validate_key<VcMaxList>(psio::to_key(loose)).ok());
   CHECK_FALSE(psio::validate_json<VcMaxList>(psio::to_json(loose)).ok());
   CHECK_FALSE(psio::validate_flatbuf<VcMaxList>(psio::to_flatbuf(loose)).ok());
   CHECK_FALSE(psio::validate_capnp<VcMaxList>(psio::to_capnp(loose)).ok());
   CHECK_FALSE(psio::validate_protobuf<VcMaxList>(psio::to_protobuf(loose)).ok());
}

TEST_CASE("value contract: the conformance value is at its limits", "[value_contract]")
{
   CHECK(psio::validate_flatbuf<Bounded>(psio::to_flatbuf(conformance_bounded())).ok());
   CHECK(psio::from_flatbuf<Bounded>(psio::to_flatbuf(conformance_bounded())) ==
         conformance_bounded());
   CHECK(psio::validate_protobuf<PbBounded>(psio::to_protobuf(pb_bounded_at_limits())).ok());
   CHECK(psio::from_protobuf<PbBounded>(psio::to_protobuf(pb_bounded_at_limits())) ==
         pb_bounded_at_limits());
   CHECK(psio::check_value_contract(conformance_bounded(), "x", "x").ok());
   CHECK(psio::check_value_contract(Bounded{}, "x", "x").ok());
}
