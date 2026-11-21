// Phase 8 — fracpack (frac32 / frac16) format tags.

#include <psio/frac.hpp>
#include <psio/reflect.hpp>
#include "frac_legacy_fixtures.hpp"

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct FracPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(FracPoint, x, y)

struct FracPerson
{
   std::int32_t id;
   std::string  name;
};
PSIO_REFLECT(FracPerson, id, name)

struct FracPacket
{
   std::uint16_t             version;
   std::vector<std::int32_t> payload;
   std::string               tag;
};
PSIO_REFLECT(FracPacket, version, payload, tag)

TEMPLATE_TEST_CASE("frac round-trips primitives", "[frac][primitive]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   auto b  = psio::encode(F{}, std::int32_t{-42});
   REQUIRE(b.size() == 4);
   auto v = psio::decode<std::int32_t>(F{}, std::span<const char>{b});
   REQUIRE(v == -42);
}

TEMPLATE_TEST_CASE("frac round-trips std::vector of fixed elements",
                   "[frac][vector]", psio::frac16, psio::frac32)
{
   using F = TestType;
   std::vector<std::uint32_t> v{1, 2, 3, 4};
   auto                       b = psio::encode(F{}, v);
   auto back = psio::decode<std::vector<std::uint32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back == v);
}

TEMPLATE_TEST_CASE("frac round-trips std::string inside a record",
                   "[frac][record][string]", psio::frac16, psio::frac32)
{
   using F = TestType;
   FracPerson pp{1, "bob"};
   auto       b = psio::encode(F{}, pp);
   auto back = psio::decode<FracPerson>(F{}, std::span<const char>{b});
   REQUIRE(back.id == 1);
   REQUIRE(back.name == "bob");
}

TEMPLATE_TEST_CASE("frac round-trips fixed records with correct header",
                   "[frac][record]", psio::frac16, psio::frac32)
{
   using F = TestType;
   FracPoint p{10, 20};
   auto      b = psio::encode(F{}, p);
   // v1 fracpack record header is always u16 regardless of W; fixed
   // region is the two int32 fields. Total = 2 + 8.
   REQUIRE(b.size() == 2u + 8u);
   auto back = psio::decode<FracPoint>(F{}, std::span<const char>{b});
   REQUIRE(back.x == 10);
   REQUIRE(back.y == 20);
}

TEMPLATE_TEST_CASE("frac round-trips records with multiple variable fields",
                   "[frac][record][variable]", psio::frac16, psio::frac32)
{
   using F = TestType;
   FracPacket pk{7, {100, 200, 300}, "hello"};
   auto       b = psio::encode(F{}, pk);
   auto back = psio::decode<FracPacket>(F{}, std::span<const char>{b});
   REQUIRE(back.version == 7);
   REQUIRE(back.payload == std::vector<std::int32_t>{100, 200, 300});
   REQUIRE(back.tag == "hello");
}

TEMPLATE_TEST_CASE("frac round-trips std::optional<T>", "[frac][optional]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   std::optional<std::int32_t> some = 123;
   auto                        b    = psio::encode(F{}, some);
   auto back = psio::decode<std::optional<std::int32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back.has_value());
   REQUIRE(*back == 123);

   std::optional<std::int32_t> none;
   auto                        b2 = psio::encode(F{}, none);
   auto back2 = psio::decode<std::optional<std::int32_t>>(
      F{}, std::span<const char>{b2});
   REQUIRE(!back2.has_value());
}

TEMPLATE_TEST_CASE("frac size_of agrees with encode output size",
                   "[frac][size_of]", psio::frac16, psio::frac32)
{
   using F = TestType;
   FracPacket pk{1, {5, 10}, "x"};
   auto       b = psio::encode(F{}, pk);
   REQUIRE(psio::size_of(F{}, pk) == b.size());
}

TEST_CASE("frac scoped sugar works", "[frac][format_tag_base]")
{
   FracPoint p{4, 5};
   auto      a = psio::encode(psio::frac32{}, p);
   auto      b = psio::frac32::encode(p);
   REQUIRE(a == b);

   auto v = psio::frac32::decode<FracPoint>(std::span<const char>{a});
   REQUIRE(v.x == 4);
   REQUIRE(v.y == 5);
}

// vector<variable-element> exercises the offset-table walker ported
// from psio1 (frac.hpp encode/decode/validate at the
// is_std_vector_v<T>+!is_fixed_v<E> branch).

TEMPLATE_TEST_CASE("frac round-trips vector<string>",
                   "[frac][vector][variable]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   std::vector<std::string> v{"alpha", "", "bravo", "charlie-delta"};
   auto                     b = psio::encode(F{}, v);
   auto back = psio::decode<std::vector<std::string>>(
      F{}, std::span<const char>{b});
   REQUIRE(back == v);
   REQUIRE(psio::size_of(F{}, v) == b.size());
}

TEMPLATE_TEST_CASE("frac round-trips vector<record-with-variable-field>",
                   "[frac][vector][variable]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   std::vector<FracPerson> v{{1, "alice"}, {2, ""}, {3, "carol"}};
   auto                    b = psio::encode(F{}, v);
   auto back = psio::decode<std::vector<FracPerson>>(
      F{}, std::span<const char>{b});
   REQUIRE(back.size() == 3);
   REQUIRE(back[0].id == 1);
   REQUIRE(back[0].name == "alice");
   REQUIRE(back[1].id == 2);
   REQUIRE(back[1].name == "");
   REQUIRE(back[2].id == 3);
   REQUIRE(back[2].name == "carol");
   REQUIRE(psio::size_of(F{}, v) == b.size());
}

TEMPLATE_TEST_CASE("frac round-trips empty vector<variable-element>",
                   "[frac][vector][variable]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   std::vector<std::string> v;
   auto                     b    = psio::encode(F{}, v);
   auto                     back = psio::decode<std::vector<std::string>>(
      F{}, std::span<const char>{b});
   REQUIRE(back.empty());
   REQUIRE(psio::size_of(F{}, v) == b.size());
}

TEMPLATE_TEST_CASE("frac validates good vector<variable-element>",
                   "[frac][vector][variable][validate]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   std::vector<std::string> v{"x", "yy", "zzz"};
   auto                     b  = psio::encode(F{}, v);
   auto                     st = psio::validate<std::vector<std::string>>(
      F{}, std::span<const char>{b});
   REQUIRE(st.ok());
}

TEMPLATE_TEST_CASE("frac validates rejects truncated vector<variable>",
                   "[frac][vector][variable][validate]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   std::vector<std::string> v{"x", "yy", "zzz"};
   auto                     b = psio::encode(F{}, v);
   // Drop the last byte to make the last element's payload OOB.
   b.pop_back();
   auto st = psio::validate<std::vector<std::string>>(
      F{}, std::span<const char>{b});
   REQUIRE(!st.ok());
}

static std::vector<char> legacy_bytes(std::string_view name)
{
   for (const auto& row : frac_legacy_fixtures::data)
      if (row.name == name)
      {
         return {row.bytes.begin(), row.bytes.end()};
      }
   FAIL("Missing legacy golden fixture");
   return {};
}

template <typename T>
static void require_legacy(std::string_view name, const T& value)
{
   CAPTURE(name);
   const auto expected = legacy_bytes(name);
   const auto bytes = psio::encode(psio::frac32{}, value);
   CHECK(bytes == expected);
   CHECK(psio::size_of(psio::frac32{}, value) == expected.size());
   REQUIRE(psio::validate<T>(psio::frac32{}, expected).ok());
   CHECK(psio::decode<T>(psio::frac32{}, expected) == value);
   CHECK(psio::encode(psio::frac32{}, psio::decode<T>(psio::frac32{}, expected)) == expected);
   for (std::size_t size = 0; size < expected.size(); ++size)
      CHECK_FALSE(psio::validate<T>(psio::frac32{}, std::span<const char>{expected}.first(size)).ok());
}

TEST_CASE("frac32 matches genuine legacy optional suffix goldens", "[frac][legacy][optional]")
{
   require_legacy("zero", CompatBase{"hello"});
   require_legacy("one-none", CompatOne{"hello",std::nullopt});
   require_legacy("one-empty", CompatOne{"hello",std::string{}});
   require_legacy("one-some", CompatOne{"hello",std::string{"memo"}});
   require_legacy("two-none", CompatMany{"hello",std::nullopt,std::nullopt});
   require_legacy("two-first-some", CompatMany{"hello",std::string{"memo"},std::nullopt});
   require_legacy("two-last-some", CompatMany{"hello",std::nullopt,9u});
   require_legacy("two-both-some", CompatMany{"hello",std::string{"memo"},9u});
   require_legacy("interior-none", CompatInterior{"hello",std::nullopt,9u});
   require_legacy("all-none", CompatAllOptional{});
   require_legacy("all-last-some", CompatAllOptional{std::nullopt,9u});
   require_legacy("frozen-none", CompatFrozen{});
}

TEST_CASE("frac32 matches genuine SDK SignedTransaction bytes", "[frac][legacy][transaction]")
{
   const CompatTransaction trx{{123,0x01020304,1,5}, {{1,2,3,{4,0,7,0,0,0}}}, {{4,{'a','b'}}}};
   require_legacy("transaction", trx);
   CompatSignedTransaction signed_trx{psio::encode(psio::frac32{}, trx), {{'p'}}, std::nullopt};
   require_legacy("signed-none", signed_trx);
   signed_trx.subjectiveData = std::vector<std::vector<char>>{};
   require_legacy("signed-empty", signed_trx);
   signed_trx.subjectiveData = std::vector<std::vector<char>>{{'s','u','b'}};
   require_legacy("signed-some", signed_trx);
   const auto decoded = psio::decode<CompatSignedTransaction>(psio::frac32{}, legacy_bytes("signed-none"));
   REQUIRE(psio::validate<CompatTransaction>(psio::frac32{}, decoded.transaction).ok());
   CHECK(psio::decode<CompatTransaction>(psio::frac32{}, decoded.transaction) == trx);
}

TEST_CASE("frac32 reads omitted known optionals and unknown suffixes", "[frac][legacy][evolution]")
{
   for (auto name : {"zero", "one-none", "two-none"})
   {
      const auto bytes = legacy_bytes(name);
      REQUIRE(psio::validate<CompatMany>(psio::frac32{}, bytes).ok());
      CHECK(psio::validate_strict<CompatMany>(psio::frac32{}, bytes).ok());
      const auto value = psio::decode<CompatMany>(psio::frac32{}, bytes);
      CHECK(value.text == "hello");
      CHECK(!value.memo);
      CHECK(!value.count);
   }
   for (auto name : {"one-empty", "one-some", "two-last-some", "two-both-some"})
   {
      const auto bytes = legacy_bytes(name);
      REQUIRE(psio::validate<CompatBase>(psio::frac32{}, bytes).ok());
      CHECK_FALSE(psio::validate_strict<CompatBase>(psio::frac32{}, bytes).ok());
      CHECK(psio::decode<CompatBase>(psio::frac32{}, bytes).text == "hello");
   }
   const auto bytes = legacy_bytes("two-last-some");
   REQUIRE(psio::validate<CompatOne>(psio::frac32{}, bytes).ok());
   CHECK(!psio::decode<CompatOne>(psio::frac32{}, bytes).memo);
   const CompatOuterWriter outer{{"hello",std::string{"memo"},9u},"sibling"};
   const auto nested = psio::encode(psio::frac32{}, outer);
   REQUIRE(psio::validate<CompatOuterReader>(psio::frac32{}, nested).ok());
   CHECK_FALSE(psio::validate_strict<CompatOuterReader>(psio::frac32{}, nested).ok());
   const auto older = psio::decode<CompatOuterReader>(psio::frac32{}, nested);
   CHECK(older.child.text == "hello");
   CHECK(older.sibling == "sibling");
   const std::vector<CompatOne> pruned{{"hello",std::nullopt},{"next",std::string{}}};
   const auto sequence = psio::encode(psio::frac32{}, pruned);
   REQUIRE(psio::validate<std::vector<CompatOne>>(psio::frac32{}, sequence).ok());
   CHECK(psio::decode<std::vector<CompatOne>>(psio::frac32{}, sequence) == pruned);
   const auto newer_sequence = psio::encode(psio::frac32{}, std::vector<CompatMany>{{"hello",std::string{"memo"},9u}});
   REQUIRE(psio::validate<std::vector<CompatBase>>(psio::frac32{}, newer_sequence).ok());
   CHECK_FALSE(psio::validate_strict<std::vector<CompatBase>>(psio::frac32{}, newer_sequence).ok());
}

TEMPLATE_TEST_CASE("frac optional suffix helpers retain both word widths", "[frac][optional]",
                   psio::frac16, psio::frac32)
{
   using F = TestType;
   CompatMany value{"hello",std::nullopt,std::nullopt};
   const auto bytes = psio::encode(F{}, value);
   CHECK(bytes == psio::encode(F{}, CompatBase{"hello"}));
   REQUIRE(psio::validate<CompatMany>(F{}, bytes).ok());
   CHECK(psio::decode<CompatMany>(F{}, bytes) == value);
}

TEST_CASE("frac32 rejects malformed and nonminimal optional layouts", "[frac][legacy][validate]")
{
   auto bytes = legacy_bytes("one-some");
   bytes[0] = static_cast<char>(0xff);
   bytes[1] = static_cast<char>(0xff);
   CHECK(!psio::validate<CompatOne>(psio::frac32{}, bytes).ok());
   CHECK(!psio::validate<CompatBase>(psio::frac32{}, std::vector<char>{0,0}).ok());
   const std::vector<char> explicit_none{8,0,8,0,0,0,1,0,0,0,5,0,0,0,'h','e','l','l','o'};
   CHECK(!psio::validate<CompatOne>(psio::frac32{}, explicit_none).ok());
   CHECK(!psio::validate<CompatBase>(psio::frac32{}, explicit_none).ok());
   auto partial = legacy_bytes("one-empty");
   partial[0] = 6;
   CHECK(!psio::validate<CompatOne>(psio::frac32{}, partial).ok());
   const std::vector<char> unaligned_extension{5,0,5,0,0,0,0,5,0,0,0,'h','e','l','l','o'};
   CHECK_FALSE(psio::validate<CompatBase>(psio::frac32{}, unaligned_extension).ok());
   auto scalar_zero = legacy_bytes("all-last-some");
   scalar_zero[6] = 0;
   CHECK(!psio::validate<CompatAllOptional>(psio::frac32{}, scalar_zero).ok());
   for (unsigned offset : {1,4,14,255})
   {
      auto unknown = legacy_bytes("two-last-some");
      unknown[10] = static_cast<char>(offset);
      CHECK(!psio::validate<CompatBase>(psio::frac32{}, unknown).ok());
   }
   auto trailing = legacy_bytes("zero");
   trailing.push_back(0);
   CHECK(!psio::validate<CompatBase>(psio::frac32{}, trailing).ok());
}

struct CompatCapped { std::string text; };
PSIO_REFLECT(CompatCapped, text, maxDynamicData(16))
TEST_CASE("frac strict retains the structural size cap before semantic decode", "[frac][legacy][strict]")
{
   const auto bytes = psio::encode(psio::frac32{}, CompatBase{std::string(40,'x')});
   CHECK_FALSE(psio::validate<CompatCapped>(psio::frac32{}, bytes).ok());
   CHECK_FALSE(psio::validate_strict<CompatCapped>(psio::frac32{}, bytes).ok());
}
