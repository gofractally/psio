// Tests for psio/schema_builder.hpp — C++ reflection → Schema IR.

#include <psio/schema_builder.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz.hpp>

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

using LabeledSchemaVariant=std::variant<std::uint16_t,bool>;
template<> struct psio::schema_variant_labels<LabeledSchemaVariant>
{
   static constexpr std::array<std::string_view,2> names{"Code","Ready"};
};

namespace recursive_schema
{
   struct Node
   {
      std::uint8_t value=0;
      std::optional<psio::schema_types::Box<Node>> next;
   };
   PSIO_REFLECT(Node,value,next)
}

TEST_CASE("SchemaBuilder emits actual tuple and ordered variant IR", "[schema_builder][pssz]")
{
   namespace ir=psio::schema_types;namespace codec=psio::pssz_ir;
   psio::SchemaBuilder builder;
   auto zero=builder.insert<std::tuple<>>();
   auto pair=builder.insert<std::tuple<std::uint32_t,std::string>>();
   auto choice=builder.insert<std::variant<std::uint32_t,std::string>>();
   auto named=builder.insert<LabeledSchemaVariant>();
   auto schema=std::move(builder).build();
   REQUIRE(std::get<ir::Tuple>(zero.resolve(schema)->value).members.empty());
   REQUIRE(std::get<ir::Tuple>(pair.resolve(schema)->value).members.size()==2);
   const auto& variants=std::get<ir::Variant>(choice.resolve(schema)->value);
   REQUIRE(variants.members[0].name=="0");REQUIRE(variants.members[1].name=="1");
   const auto& labeled=std::get<ir::Variant>(named.resolve(schema)->value);
   REQUIRE(labeled.members[0].name=="Code");REQUIRE(labeled.members[1].name=="Ready");
   std::vector<char> bytes;codec::Value decoded;
   REQUIRE(codec::encode<4>(schema,zero,codec::Sequence{},bytes).ok());
   REQUIRE(bytes==psio::encode(psio::pssz32{},std::tuple{}));bytes.clear();
   REQUIRE(codec::encode<4>(schema,pair,codec::Sequence{codec::Unsigned{7},std::string("hi")},bytes).ok());
   REQUIRE(bytes==psio::encode(psio::pssz32{},std::tuple{std::uint32_t{7},std::string("hi")}));
   bytes.clear();auto value=codec::Value::variant(1,std::string("hello"));
   REQUIRE(codec::encode<4>(schema,choice,value,bytes).ok());
   REQUIRE(bytes==psio::encode(psio::pssz32{},std::variant<std::uint32_t,std::string>{std::in_place_index<1>,"hello"}));
   REQUIRE(codec::decode_exact<4>(schema,choice,bytes,decoded).ok());REQUIRE(decoded==value);
}

TEST_CASE("SchemaBuilder emits count bounds and packed bit metadata", "[schema_builder][bounds]")
{
   namespace ir=psio::schema_types;
   psio::SchemaBuilder builder;
   auto list=builder.insert<psio::bounded<std::uint16_t,5>>();
   auto text=builder.insert<psio::utf8_string<8>>();
   auto bits=builder.insert<psio::bitvector<9>>();
   auto bitlist=builder.insert<psio::bitlist<17>>();
   auto schema=std::move(builder).build();
   REQUIRE(std::get<ir::BoundedList>(list.resolve(schema)->value).maxCount==5);
   const auto& string=std::get<ir::Custom>(text.resolve(schema)->value);
   REQUIRE(string.id=="utf8");REQUIRE(std::get<ir::BoundedList>(string.type->value).maxCount==8);
   const auto& vector=std::get<ir::Custom>(bits.resolve(schema)->value);
   REQUIRE(vector.id=="bitvector");const auto& array=std::get<ir::Array>(vector.type->value);
   REQUIRE(array.len==9);REQUIRE(std::get<ir::Int>(array.type->value).bits==1);
   const auto& list_bits=std::get<ir::Custom>(bitlist.resolve(schema)->value);
   REQUIRE(list_bits.id=="bitlist");REQUIRE(std::get<ir::BoundedList>(list_bits.type->value).maxCount==17);
}

TEST_CASE("SchemaBuilder preserves recursive graphs through transparent boxes", "[schema_builder][recursive]")
{
   namespace ir=psio::schema_types;namespace codec=psio::pssz_ir;
   psio::SchemaBuilder builder;auto root=builder.insert<recursive_schema::Node>();
   auto schema=std::move(builder).build();
   const auto& object=std::get<ir::Object>(root.resolve(schema)->value);
   const auto& next=std::get<ir::Option>(object.members[1].type->resolve(schema)->value);
   // The box is recorded (Custom "box": the inner type on the wire, with no
   // inferred size bound) and leads back to the same record.
   const auto& boxed=std::get<ir::Custom>(next.type->resolve(schema)->value);
   REQUIRE(boxed.id=="box");
   REQUIRE(boxed.type->resolve(schema)==root.resolve(schema));
   recursive_schema::Node value{1,ir::Box<recursive_schema::Node>{recursive_schema::Node{2,std::nullopt}}};
   auto bytes=psio::encode(psio::pssz32{},value);codec::Value decoded;
   REQUIRE(codec::decode_exact<4>(schema,root,bytes,decoded).ok());
   std::vector<char> actual;REQUIRE(codec::encode<4>(schema,root,decoded,actual).ok());REQUIRE(actual==bytes);
}

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
