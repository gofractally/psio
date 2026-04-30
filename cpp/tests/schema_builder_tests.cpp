// Tests for psio/schema_builder.hpp — C++ reflection → Schema IR.

#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace test_sb
{
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
   };
   PSIO_REFLECT(Point, x, y)

   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;
      std::optional<std::int32_t> count;
   };
   PSIO_REFLECT(Bag, name, payload, count)
}  // namespace test_sb

using test_sb::Bag;
using test_sb::Point;

// ─── Primitives + records ────────────────────────────────────────────

TEST_CASE("SchemaBuilder: primitive Int / Float / string", "[schema_builder]")
{
   using namespace psio::schema_types;

   auto s = psio::SchemaBuilder{}
               .insert<std::uint32_t>("u32")
               .insert<double>("f64")
               .insert<std::string>("string")
               .build();

   const auto* u32 = s.get("u32");
   REQUIRE(u32 != nullptr);
   const auto* u32_t = u32->resolve(s);
   REQUIRE(u32_t != nullptr);
   REQUIRE(std::holds_alternative<Int>(u32_t->value));
   CHECK(std::get<Int>(u32_t->value).bits == 32);
   CHECK_FALSE(std::get<Int>(u32_t->value).isSigned);

   const auto* f64 = s.get("f64")->resolve(s);
   REQUIRE(std::holds_alternative<Float>(f64->value));
   CHECK(std::get<Float>(f64->value).mantissa == 53);

   // string is wrapped: Custom{ List{u8}, "string" }
   const auto* str = s.get("string")->resolve(s);
   REQUIRE(std::holds_alternative<Custom>(str->value));
   CHECK(std::get<Custom>(str->value).id == "string");
}

TEST_CASE("SchemaBuilder: PSIO_REFLECT'd struct → Object",
          "[schema_builder]")
{
   using namespace psio::schema_types;

   auto s = psio::SchemaBuilder{}.insert<Point>("Point").build();

   const auto* p = s.get("Point")->resolve(s);
   REQUIRE(std::holds_alternative<Object>(p->value));
   const auto& o = std::get<Object>(p->value);

   REQUIRE(o.members.size() == 2);
   CHECK(o.members[0].name == "x");
   CHECK(o.members[1].name == "y");

   const auto* x_t = o.members[0].type->resolve(s);
   REQUIRE(std::holds_alternative<Int>(x_t->value));
   CHECK(std::get<Int>(x_t->value).bits == 32);
   CHECK(std::get<Int>(x_t->value).isSigned);
}

TEST_CASE("SchemaBuilder: vector / optional / string in a record",
          "[schema_builder]")
{
   using namespace psio::schema_types;

   auto s = psio::SchemaBuilder{}.insert<Bag>("Bag").build();

   const auto* b = s.get("Bag")->resolve(s);
   const auto& o = std::get<Object>(b->value);
   REQUIRE(o.members.size() == 3);

   // payload: vector<u8> — vector<unsigned char> takes the hex Custom
   // path; vector<uint8_t> alone routes via the generic vector branch.
   // Either way the inner element resolves to Int{8, false}.
   const auto* payload = o.members[1].type->resolve(s);
   const Int*  inner   = nullptr;
   if (auto* lst = std::get_if<List>(&payload->value))
   {
      const auto* e = lst->type->resolve(s);
      inner         = std::get_if<Int>(&e->value);
   }
   else if (auto* cus = std::get_if<Custom>(&payload->value))
   {
      const auto* lst = cus->type->resolve(s);
      const auto* lst_node = std::get_if<List>(&lst->value);
      REQUIRE(lst_node != nullptr);
      const auto* e = lst_node->type->resolve(s);
      inner         = std::get_if<Int>(&e->value);
   }
   REQUIRE(inner != nullptr);
   CHECK(inner->bits == 8);
   CHECK_FALSE(inner->isSigned);

   // count: optional<i32>
   const auto* count = o.members[2].type->resolve(s);
   REQUIRE(std::holds_alternative<Option>(count->value));
   const auto* count_inner =
      std::get<Option>(count->value).type->resolve(s);
   REQUIRE(std::holds_alternative<Int>(count_inner->value));
}

// ─── Type deduplication: same T twice → same name reference ──────────

TEST_CASE("SchemaBuilder: repeated insert<T> reuses the first name",
          "[schema_builder]")
{
   using namespace psio::schema_types;

   psio::SchemaBuilder b;
   auto first  = b.insert<Point>();
   auto second = b.insert<Point>();

   REQUIRE(std::holds_alternative<Type>(first.value));
   REQUIRE(std::holds_alternative<Type>(second.value));
   CHECK(std::get<Type>(first.value).type ==
         std::get<Type>(second.value).type);
}

// ─── Interface walk ─────────────────────────────────────────────────

namespace test_sb
{
   struct datetime
   {
      std::uint64_t seconds = 0;
   };
   PSIO_REFLECT(datetime, seconds)
}  // namespace test_sb
using test_sb::datetime;

struct sb_wall_clock
{
   static datetime now();
   static datetime resolution();
};

PSIO_PACKAGE(sb_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(sb_clocks)

PSIO_INTERFACE(sb_wall_clock,
               types(datetime),
               funcs(func(now), func(resolution)))

TEST_CASE("SchemaBuilder: insert_interface populates name + funcs + types",
          "[schema_builder][interface]")
{
   auto s = psio::SchemaBuilder{}
               .insert_interface<sb_wall_clock>()
               .build();

   CHECK(s.package.name == "sb_clocks");
   CHECK(s.package.version == "0.2.3");

   REQUIRE(s.interfaces.size() == 1);
   const auto& iface = s.interfaces[0];
   CHECK(iface.name == "sb_wall_clock");
   REQUIRE(iface.type_names.size() == 1);
   REQUIRE(iface.funcs.size() == 2);
   CHECK(iface.funcs[0].name == "now");
   CHECK(iface.funcs[1].name == "resolution");

   // Each func returns datetime — should resolve to the same Object.
   REQUIRE(iface.funcs[0].result.has_value());
   const auto* r = iface.funcs[0].result->get()->resolve(s);
   REQUIRE(std::holds_alternative<psio::schema_types::Object>(r->value));
}

// ─── World walk: imports (use + interface) + exports ────────────────

PSIO_USE(sb_clocks, sb_wall_clock, "0.2.3")

struct sb_random
{
   static std::uint64_t get_random_u64();
};

PSIO_INTERFACE(sb_random, types(), funcs(get_random_u64))

PSIO_WORLD(sb_test_world,
           imports(psio::detail::PSIO_USE_TAG_(sb_clocks, sb_wall_clock)),
           exports(sb_random))

TEST_CASE("SchemaBuilder: insert_world walks use-imports + interface-exports",
          "[schema_builder][world]")
{
   auto s = psio::SchemaBuilder{}
               .insert_world<psio::detail::sb_test_world_world_tag>()
               .build();

   REQUIRE(s.worlds.size() == 1);
   const auto& w = s.worlds[0];
   CHECK(w.name == "sb_test_world");

   // Import is a Use, captured as both a Schema::uses entry and a
   // World::imports UseRef.
   REQUIRE(s.uses.size() == 1);
   CHECK(s.uses[0].package == "sb_clocks");
   CHECK(s.uses[0].interface_name == "sb_wall_clock");
   REQUIRE(w.imports.size() == 1);
   CHECK(w.imports[0].package == "sb_clocks");

   // Export is an interface tag — walked into Schema::interfaces.
   REQUIRE(w.exports.size() == 1);
   CHECK(w.exports[0] == "sb_random");

   bool found_random = false;
   for (const auto& iface : s.interfaces)
      if (iface.name == "sb_random")
         found_random = true;
   CHECK(found_random);
}
