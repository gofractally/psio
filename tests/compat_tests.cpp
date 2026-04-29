// Phase 17 — compat layer.
//
// Validates that including psio/compat/psio_aliases.hpp gives existing
// consumers the `psio1::` spellings they used against psio v1, now
// resolving to psio. Once Phase 18 renames psio → psio, this header
// disappears and callsites keep working verbatim.

#include <psio/compat/psio_aliases.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Use the old PSIO1_REFLECT spelling — the macro alias re-routes.
struct CompatPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO1_REFLECT(CompatPoint, x, y)

struct CompatPerson
{
   std::string                 name;
   std::int32_t                age;
   std::vector<std::uint32_t>  scores;
};
PSIO1_REFLECT(CompatPerson, name, age, scores)

TEST_CASE("psio1:: namespace alias routes encode/decode through psio",
          "[compat]")
{
   CompatPoint p{4, 7};
   auto        bytes = psio1::encode(psio1::ssz{}, p);
   auto        back =
      psio1::decode<CompatPoint>(psio1::ssz{}, std::span<const char>{bytes});
   REQUIRE(back.x == 4);
   REQUIRE(back.y == 7);
}

TEST_CASE("psio1::reflect<T> resolves through the namespace alias",
          "[compat][reflect]")
{
   STATIC_REQUIRE(psio1::reflect<CompatPoint>::member_count == 2);
   STATIC_REQUIRE(psio1::reflect<CompatPoint>::member_name<0> == "x");
   STATIC_REQUIRE(psio1::reflect<CompatPoint>::member_name<1> == "y");
}

TEST_CASE("psio1:: identity with psio:: — they name the same entities",
          "[compat]")
{
   // Tag types are the same struct regardless of namespace spelling.
   STATIC_REQUIRE(std::is_same_v<psio1::ssz, psio::ssz>);
   STATIC_REQUIRE(std::is_same_v<psio1::json, psio::json>);
   STATIC_REQUIRE(std::is_same_v<psio1::frac32, psio::frac32>);
}

TEST_CASE("psio1::schema_of routes to psio schema emitter",
          "[compat][schema]")
{
   auto sc = psio1::schema_of<CompatPerson>();
   REQUIRE(sc.is_record());
   REQUIRE(sc.as_record().fields.size() == 3);
   REQUIRE(sc.as_record().fields[0].name == "name");
}

TEST_CASE("psio1::transcode is callable under the aliased namespace",
          "[compat][transcode]")
{
   CompatPerson pp{"jack", 19, {1, 2, 3}};
   auto         sc = psio1::schema_of<CompatPerson>();
   auto         ssz_bytes = psio1::encode(psio1::ssz{}, pp);
   auto         json_str  = psio1::transcode(
      psio1::ssz{}, sc, std::span<const char>{ssz_bytes}, psio1::json{});

   REQUIRE(json_str.find("\"name\":\"jack\"") != std::string::npos);
   REQUIRE(json_str.find("\"age\":19") != std::string::npos);
}

TEST_CASE("Mixed psio1:: and psio:: call sites share state",
          "[compat]")
{
   CompatPoint p{11, 22};
   auto        ssz_bytes_new = psio::encode(psio::ssz{}, p);
   auto        ssz_bytes_old = psio1::encode(psio1::ssz{}, p);
   REQUIRE(ssz_bytes_new == ssz_bytes_old);
}
