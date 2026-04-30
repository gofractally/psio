// Phase 14b — dynamic_value + schema-driven JSON codec.

#include <psio/dynamic_json.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/reflect.hpp>
#include <psio/schema.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct DynPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(DynPoint, x, y)

struct DynPerson
{
   std::string                 name;
   std::int32_t                age;
   std::vector<std::uint32_t>  scores;
   std::optional<std::string>  note;
};
PSIO_REFLECT(DynPerson, name, age, scores, note)

TEST_CASE("to_dynamic preserves primitives", "[dynamic][primitive]")
{
   auto dv = psio::to_dynamic(std::int32_t{42});
   REQUIRE(dv.holds<std::int32_t>());
   REQUIRE(dv.as<std::int32_t>() == 42);
}

TEST_CASE("to_dynamic + from_dynamic round-trips a reflected record",
          "[dynamic][record]")
{
   DynPoint p{3, 5};
   auto     dv = psio::to_dynamic(p);
   REQUIRE(dv.holds<psio::dynamic_record>());
   REQUIRE(dv.as<psio::dynamic_record>().fields.size() == 2);

   auto back = psio::from_dynamic<DynPoint>(dv);
   REQUIRE(back.x == 3);
   REQUIRE(back.y == 5);
}

TEST_CASE("to_dynamic handles vectors and optionals", "[dynamic][composite]")
{
   DynPerson pp{"alice", 30, {1, 2, 3}, std::string{"vip"}};
   auto      dv = psio::to_dynamic(pp);
   auto      back = psio::from_dynamic<DynPerson>(dv);
   REQUIRE(back.name == "alice");
   REQUIRE(back.age == 30);
   REQUIRE(back.scores == std::vector<std::uint32_t>{1, 2, 3});
   REQUIRE(back.note.has_value());
   REQUIRE(*back.note == "vip");
}

TEST_CASE("json_encode_dynamic matches the static JSON codec",
          "[dynamic][json]")
{
   DynPoint p{11, -22};
   auto     dv = psio::to_dynamic(p);
   auto     sc = psio::schema_of<DynPoint>();
   auto     s  = psio::json_encode_dynamic(sc, dv);
   REQUIRE(s == R"({"x":11,"y":-22})");
}

TEST_CASE("json_decode_dynamic reads json through a schema",
          "[dynamic][json]")
{
   std::string text = R"({"x":7,"y":8})";
   auto        sc   = psio::schema_of<DynPoint>();
   auto        dv   = psio::json_decode_dynamic(sc, std::span<const char>{text});
   auto        back = psio::from_dynamic<DynPoint>(dv);
   REQUIRE(back.x == 7);
   REQUIRE(back.y == 8);
}

TEST_CASE("dynamic JSON round-trip preserves nested types",
          "[dynamic][json][nested]")
{
   DynPerson pp{"bob", 25, {10, 20}, std::nullopt};
   auto      sc = psio::schema_of<DynPerson>();
   auto      dv = psio::to_dynamic(pp);

   auto s    = psio::json_encode_dynamic(sc, dv);
   auto dv2  = psio::json_decode_dynamic(sc, std::span<const char>{s});
   auto back = psio::from_dynamic<DynPerson>(dv2);

   REQUIRE(back.name == "bob");
   REQUIRE(back.age == 25);
   REQUIRE(back.scores == std::vector<std::uint32_t>{10, 20});
   REQUIRE(!back.note.has_value());
}
