// What a type declares about its values is enforced by the validating read
// paths of the formats whose bytes follow another specification:
// FlatBuffers, Cap'n Proto, protobuf (psio/value_contract.hpp;
// gofractally/psio#14). A value outside a member's bound has the same
// verdict in every format.
//
// Each buffer is written from a twin type — the same members without the
// declarations, what a writer that does not know them produces — and read as
// the declared type.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/bson.hpp>
#include <psio/buffer.hpp>
#include <psio/capnp.hpp>
#include <psio/flatbuf.hpp>
#include <psio/flatbuf_view.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/msgpack.hpp>
#include <psio/nested.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/ssz.hpp>
#include <psio/wit.hpp>
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
   friend bool               operator==(const VcEven&, const VcEven&) = default;
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
   CHECK_THROWS_AS(psio::to_pssz_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_key_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_json_checked(over), psio::codec_exception);
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

// ── The two write modes, and the read side of every format ─────────────────
//
// docs/annotations.md "Reading and writing: the two modes". For every
// format: the ordinary write (psio::encode / to_F) is FAST and writes a value
// that violates what its type declares; the VALIDATED write
// (psio::encode_checked / encode_F_checked / to_F_checked) refuses it with
// the message of the format's validating reader and writes nothing; the
// validating reader refuses the bytes the fast write produced.
namespace
{
   template <typename Fmt, typename T, typename L, typename V>
   void two_modes(const V& bad, const V& good, std::string_view error)
   {
      L loose_bad;
      loose_bad.v = bad;
      T declared_bad, declared_good;
      declared_bad.v  = bad;
      declared_good.v = good;

      // Fast: the value is written, and the bytes are the ones a writer
      // that does not know the declaration produces.
      const auto bytes = psio::encode(Fmt{}, declared_bad);
      CHECK(std::string_view(bytes.data(), bytes.size()) ==
            [&] {
               const auto b = psio::encode(Fmt{}, loose_bad);
               return std::string(b.data(), b.size());
            }());

      // The validating reader refuses those bytes; the declaration decides.
      const std::span<const char> span{bytes.data(), bytes.size()};
      REQUIRE(psio::validate<L>(Fmt{}, span).ok());
      const auto read = psio::validate<T>(Fmt{}, span);
      REQUIRE_FALSE(read.ok());
      CHECK(read.error().what == error);

      // Validated: refused with the reader's message, nothing written.
      psio::detail::helpers::checked_buffer_t<Fmt> out{'x'};
      const auto                                   written = psio::encode_checked(Fmt{}, declared_bad, out);
      REQUIRE_FALSE(written.ok());
      CHECK(written.error().what == error);
      CHECK(out.size() == 1);

      // A value inside the declaration: both modes write the same bytes.
      const auto good_bytes = psio::encode(Fmt{}, declared_good);
      REQUIRE(psio::encode_checked(Fmt{}, declared_good, out).ok());
      CHECK(std::string_view(out.data() + 1, out.size() - 1) ==
            std::string_view(good_bytes.data(), good_bytes.size()));
      CHECK(psio::validate<T>(Fmt{}, std::span<const char>{good_bytes.data(), good_bytes.size()})
               .ok());
   }

   template <typename Fmt>
   void every_kind(std::string_view length)
   {
      two_modes<Fmt, VcMaxList, VcMaxListLoose>(VcInts{1, 2, 3}, VcInts{1, 2}, length);
      two_modes<Fmt, VcMaxString, VcMaxStringLoose>(std::string{"abc"}, std::string{"ab"}, length);
      two_modes<Fmt, VcMinList, VcMinListLoose>(VcInts{1}, VcInts{1, 2}, length);
      two_modes<Fmt, VcExactList, VcExactListLoose>(VcInts{1, 2, 3}, VcInts{1, 2}, length);
      two_modes<Fmt, VcElement, VcElementLoose>(VcStrings{"ab", "abc"}, VcStrings{"ab"}, length);
      two_modes<Fmt, VcSorted, VcSortedLoose>(VcInts{2, 1}, VcInts{1, 1, 2},
                                              "sorted: elements are not in order");
      two_modes<Fmt, VcUnique, VcUniqueLoose>(
         VcInts{1, 1}, VcInts{1, 2}, "sorted: elements are not in strictly ascending order");
      two_modes<Fmt, VcEven, VcEvenLoose>(std::uint32_t{3}, std::uint32_t{4}, "VcEven: v is odd");
   }
}  // namespace

TEST_CASE("write modes: fast write, validated write and validating read, every format",
          "[value_contract][write_modes]")
{
   SECTION("bin") { every_kind<psio::bin>("bin: member length_bound violated"); }
   SECTION("ssz") { every_kind<psio::ssz>("ssz: member length_bound violated"); }
   SECTION("borsh") { every_kind<psio::borsh>("borsh: member length_bound violated"); }
   SECTION("bincode") { every_kind<psio::bincode>("bincode: member length_bound violated"); }
   SECTION("avro") { every_kind<psio::avro>("avro: member length_bound violated"); }
   SECTION("msgpack") { every_kind<psio::msgpack>("msgpack: member length_bound violated"); }
   SECTION("bson") { every_kind<psio::bson>("bson: member length_bound violated"); }
   SECTION("wit") { every_kind<psio::wit>("wit: member length_bound violated"); }
   SECTION("flatbuf") { every_kind<psio::flatbuf>("flatbuf: member length_bound violated"); }
   SECTION("capnp") { every_kind<psio::capnp>("capnp: member length_bound violated"); }
   SECTION("protobuf") { every_kind<psio::protobuf>("protobuf: member length_bound violated"); }
   SECTION("key") { every_kind<psio::key>("key: member length_bound violated"); }
   SECTION("json") { every_kind<psio::json>("json: member length_bound violated"); }
   SECTION("ijson") { every_kind<psio::ijson>("json: member length_bound violated"); }
}

TEST_CASE("write modes: the helper family of a format", "[value_contract][write_modes]")
{
   const VcMaxList over{{1, 2, 3}}, fits{{1, 2}};
   // to_F: fast.
   CHECK_NOTHROW(psio::to_pssz(over));
   CHECK_NOTHROW(psio::to_key(over));
   CHECK_NOTHROW(psio::to_json(over));
   CHECK_NOTHROW(psio::to_bin(over));
   CHECK_NOTHROW(psio::to_flatbuf(over));
   // to_F_checked(v): the bytes, or the failure of from_F.
   CHECK_THROWS_AS(psio::to_pssz_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_key_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_json_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_ijson_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_bin_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_ssz_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_borsh_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_bincode_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_avro_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_msgpack_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_bson_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_wit_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_flatbuf_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_capnp_checked(over), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_protobuf_checked(over), psio::codec_exception);
   CHECK(psio::to_pssz_checked(fits) == psio::to_pssz(fits));
   CHECK(psio::to_json_checked(fits) == psio::to_json(fits));
   CHECK(psio::to_flatbuf_checked(fits) == psio::to_flatbuf(fits));
   {
      const auto fast = psio::to_key(fits);
      CHECK(psio::to_key_checked(fits) == std::vector<char>(fast.begin(), fast.end()));
   }
   // to_F_checked(v, sink) appends or fails and leaves the sink.
   std::vector<char> sink{'x'};
   CHECK_THROWS_AS(psio::to_pssz_checked(over, sink), psio::codec_exception);
   CHECK(sink == std::vector<char>{'x'});
   psio::to_pssz_checked(fits, sink);
   CHECK(sink.size() == 1 + psio::to_pssz(fits).size());
   // encode_F_checked(v, out): the status.
   std::vector<char> out;
   CHECK(psio::encode_pssz_checked(over, out).error().what ==
         std::string_view{"pssz: member length_bound violated"});
   CHECK(psio::encode_key_checked(over, out).error().what ==
         std::string_view{"key: member length_bound violated"});
   CHECK(psio::encode_flatbuf_checked(over, out).error().what ==
         std::string_view{"flatbuf: member length_bound violated"});
   CHECK(psio::encode_bin_checked(over, out).error().what ==
         std::string_view{"bin: member length_bound violated"});
   std::string text;
   CHECK(psio::encode_json_checked(over, text).error().what ==
         std::string_view{"json: member length_bound violated"});
   CHECK(out.empty());
   CHECK(text.empty());
   // validate_value: once, for every format.
   CHECK(psio::validate_value(over).error().what ==
         std::string_view{"psio: member length_bound violated"});
   CHECK(psio::validate_value(fits).ok());
   CHECK(psio::validate_value(VcEven{3}).error().what == std::string_view{"VcEven: v is odd"});
   CHECK(psio::validate_value(std::vector<VcMaxList>{fits, over}).error().what ==
         std::string_view{"psio: member length_bound violated"});
   CHECK(psio::validate_value(psio::utf8_string<8>{"\xff"}).error().what ==
         std::string_view{"utf8_spec: invalid leading byte"});
   // What holds bytes as "a valid T" is made by the validated write.
   CHECK_THROWS_AS(psio::nested<VcMaxList>{over}, psio::codec_exception);
   CHECK_THROWS_AS(psio::pssz_buffer<VcMaxList>::from(over), psio::codec_exception);
   CHECK_NOTHROW(psio::nested<VcMaxList>{fits});
}

// ── pssz: what the fast write still refuses ────────────────────────────────
//
// A declaration can decide a pssz layout (a bound or a cap sets an offset
// width). The fast write holds nothing against the declaration, and it
// never writes a frame a trusted reader would read as another value: either
// the frame cannot be written and the write fails, or the trusted decode of
// what was written is the value (or the decode refuses it: the trusted pssz
// decode checks bounds as it reads), and the validating reader refuses it.
struct WmTags
{
   std::vector<std::string> tags;
   friend bool              operator==(const WmTags&, const WmTags&) = default;
   PSIO_REFLECT(WmTags, attr(tags, max<8> | element(utf8 | max<16>)))
};
static_assert(psio::auto_pssz_width_v<WmTags> == 1);
struct WmText
{
   std::string text;
   friend bool operator==(const WmText&, const WmText&) = default;
   PSIO_REFLECT(WmText, attr(text, utf8 | max<32>))
};
struct WmOrdered
{
   std::vector<std::uint32_t> ids;
   std::vector<std::uint8_t>  digest;
   friend bool                operator==(const WmOrdered&, const WmOrdered&) = default;
   PSIO_REFLECT(WmOrdered, attr(ids, unique | min<1> | max<4>), attr(digest, exact<4>))
};
struct WmPair
{
   std::string a, b;
   friend bool operator==(const WmPair&, const WmPair&) = default;
   PSIO_REFLECT(WmPair, attr(a, max<100>), attr(b, max<100>))
};
static_assert(psio::auto_pssz_width_v<WmPair> == 1);
struct WmCapped
{
   std::string a, b;
   friend bool operator==(const WmCapped&, const WmCapped&) = default;
   PSIO_REFLECT(WmCapped, a, b, maxDynamicData(200))
};

namespace
{
   // 0: the fast write failed; 1: it wrote a frame the validator refuses and
   // the trusted decode reads back as the value. Anything else fails here.
   template <typename T>
   int fast_write(const T& value)
   {
      REQUIRE_FALSE(psio::encode_pssz_checked(value, *std::make_unique<std::vector<char>>()).ok());
      std::vector<char> bytes;
      try
      {
         bytes = psio::to_pssz(value);
      }
      catch (const psio::codec_exception&)
      {
         return 0;
      }
      CHECK_FALSE(psio::validate_pssz<T>(bytes).ok());
      // The trusted decode of pssz checks bounds, utf8 and order as it
      // reads (docs/annotations.md 7.2): it gives the value or refuses,
      // never another value.
      std::optional<T> read;
      try
      {
         read = psio::from_pssz<T>(psio::trusted{bytes});
      }
      catch (const psio::codec_exception&)
      {
      }
      if (read)
         CHECK(*read == value);
      return 1;
   }
}  // namespace

TEST_CASE("write modes: the pssz fast write refuses only what has no frame",
          "[value_contract][write_modes][pssz]")
{
   // Semantic: written, refused by the validating reader, read back intact.
   CHECK(fast_write(WmText{std::string(33, 'x')}) == 1);          // max, inside the width
   CHECK(fast_write(WmText{"\xff\xfe"}) == 1);                    // utf8
   CHECK(fast_write(WmTags{std::vector<std::string>(9, "t")}) == 1);   // max, count
   CHECK(fast_write(WmTags{{std::string(17, 'e')}}) == 1);        // a bound below the member
   CHECK(fast_write(WmTags{{"\xff"}}) == 1);                      // utf8 below the member
   CHECK(fast_write(WmOrdered{{2, 1}, {1, 2, 3, 4}}) == 1);       // order
   CHECK(fast_write(WmOrdered{{1, 1}, {1, 2, 3, 4}}) == 1);       // uniqueness
   CHECK(fast_write(WmOrdered{{}, {1, 2, 3, 4}}) == 1);           // min
   CHECK(fast_write(WmOrdered{{1}, {1, 2, 3}}) == 1);             // exact
   CHECK(fast_write(WmOrdered{{1, 2, 3, 4, 5}, {1, 2, 3, 4}}) == 1);   // max
   CHECK(fast_write(VcEven{3}) == 1);                             // invariant
   // Far over the bound and still a frame: the last member of a record ends
   // where the record ends, so no offset has to name a position past 255.
   CHECK(fast_write(WmText{std::string(300, 'x')}) == 1);
   CHECK(fast_write(WmTags{{std::string(400, 'e')}}) == 1);
   // Structural: the offsets of WmTags and WmPair are one byte wide, and
   // these values need an offset that does not fit one.
   CHECK(fast_write(WmTags{std::vector<std::string>(40, std::string(16, 't'))}) == 0);
   CHECK(fast_write(WmPair{std::string(300, 'a'), "b"}) == 0);
   CHECK(fast_write(WmPair{"a", std::string(300, 'b')}) == 1);
   // The cap is the width budget: over it, no frame.
   CHECK(fast_write(WmCapped{std::string(150, 'a'), std::string(100, 'b')}) == 0);
   CHECK_NOTHROW(psio::to_pssz(WmCapped{std::string(100, 'a'), std::string(90, 'b')}));
}
