// Phase 14a — psio::schema + reflect-to-schema bridge.

#include <psio/reflect.hpp>
#include <psio/schema.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct SchemaPoint
{
   std::int32_t  x;
   std::int32_t  y;
};
PSIO_REFLECT(SchemaPoint, x, y)

struct SchemaPerson
{
   std::string                 name;
   std::int32_t                age;
   std::vector<std::uint32_t>  scores;
};
PSIO_REFLECT(SchemaPerson, name, age, scores)

struct SchemaNested
{
   SchemaPoint                 origin;
   std::optional<std::string>  label;
};
PSIO_REFLECT(SchemaNested, origin, label)

TEST_CASE("schema_of<bool> returns a primitive kind", "[schema][primitive]")
{
   auto s = psio::schema_of<bool>();
   REQUIRE(s.is_primitive());
   REQUIRE(s.as_primitive() == psio::primitive_kind::Bool);
}

TEST_CASE("schema_of<integer> returns matching primitive kind",
          "[schema][primitive]")
{
   REQUIRE(psio::schema_of<std::int32_t>().as_primitive() ==
           psio::primitive_kind::Int32);
   REQUIRE(psio::schema_of<std::uint64_t>().as_primitive() ==
           psio::primitive_kind::Uint64);
   REQUIRE(psio::schema_of<float>().as_primitive() ==
           psio::primitive_kind::Float32);
}

TEST_CASE("schema_of<std::string> returns String primitive",
          "[schema][primitive]")
{
   auto s = psio::schema_of<std::string>();
   REQUIRE(s.as_primitive() == psio::primitive_kind::String);
}

TEST_CASE("schema_of<std::vector<T>> returns an unbounded sequence",
          "[schema][sequence]")
{
   auto s = psio::schema_of<std::vector<std::uint32_t>>();
   REQUIRE(s.is_sequence());
   REQUIRE(!s.as_sequence().fixed_count.has_value());
   REQUIRE(s.as_sequence().element->as_primitive() ==
           psio::primitive_kind::Uint32);
}

TEST_CASE("schema_of<std::array<T, N>> returns a fixed-count sequence",
          "[schema][sequence]")
{
   auto s = psio::schema_of<std::array<std::int32_t, 4>>();
   REQUIRE(s.is_sequence());
   REQUIRE(s.as_sequence().fixed_count.has_value());
   REQUIRE(*s.as_sequence().fixed_count == 4);
   REQUIRE(s.as_sequence().element->as_primitive() ==
           psio::primitive_kind::Int32);
}

TEST_CASE("schema_of<std::optional<T>> returns an optional descriptor",
          "[schema][optional]")
{
   auto s = psio::schema_of<std::optional<std::int32_t>>();
   REQUIRE(s.is_optional());
   REQUIRE(s.as_optional().value_type->as_primitive() ==
           psio::primitive_kind::Int32);
}

TEST_CASE("schema_of<reflected record> walks the fields", "[schema][record]")
{
   auto s = psio::schema_of<SchemaPoint>();
   REQUIRE(s.is_record());
   const auto& r = s.as_record();
   REQUIRE(r.name == "SchemaPoint");
   REQUIRE(r.fields.size() == 2);
   REQUIRE(r.fields[0].name == "x");
   REQUIRE(r.fields[0].field_number == 1);
   REQUIRE(r.fields[0].type->as_primitive() == psio::primitive_kind::Int32);
   REQUIRE(r.fields[1].name == "y");
}

TEST_CASE("schema carries mixed primitive / sequence / optional fields",
          "[schema][record]")
{
   auto s = psio::schema_of<SchemaPerson>();
   REQUIRE(s.is_record());
   const auto& r = s.as_record();
   REQUIRE(r.fields.size() == 3);
   REQUIRE(r.fields[0].type->as_primitive() ==
           psio::primitive_kind::String);
   REQUIRE(r.fields[1].type->as_primitive() ==
           psio::primitive_kind::Int32);
   REQUIRE(r.fields[2].type->is_sequence());
}

TEST_CASE("schema supports nested records + optionals", "[schema][nested]")
{
   auto s = psio::schema_of<SchemaNested>();
   REQUIRE(s.is_record());
   const auto& r = s.as_record();
   REQUIRE(r.fields.size() == 2);

   REQUIRE(r.fields[0].type->is_record());
   REQUIRE(r.fields[0].type->as_record().name == "SchemaPoint");

   REQUIRE(r.fields[1].type->is_optional());
   REQUIRE(r.fields[1].type->as_optional().value_type->as_primitive() ==
           psio::primitive_kind::String);
}
