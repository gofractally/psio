// Tests for psio/schema_builder.hpp — C++ reflection → Schema IR.

#include <psio/schema_builder.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz.hpp>
#include <psio/json.hpp>
#include <psio/pssz_ir_compare.hpp>
#include <psio/pssz_ir_json.hpp>

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
template<> struct psio::variant_labels<LabeledSchemaVariant>
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
   REQUIRE(variants.members[0].name=="uint32");REQUIRE(variants.members[1].name=="string");
   REQUIRE(variants.members[0].attributes.empty());REQUIRE(variants.members[1].attributes.empty());
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

// An alternative has one label (psio/variant_label.hpp): its name in the
// schema and the key of its tagged text form.
namespace label_test
{
   struct Circle
   {
      std::uint32_t radius = 0;
      friend bool   operator==(const Circle&, const Circle&) = default;
   };
   PSIO_REFLECT(Circle, radius)
   struct Opaque  // a type with no name: not reflected, no get_type_name
   {
   };
   using Repeated = std::variant<std::uint32_t, std::string, std::string, Circle>;
   using Nameless = std::variant<Circle, Opaque>;
   using Generic  = std::variant<std::vector<std::uint32_t>, std::optional<Circle>,
                                 std::tuple<std::uint8_t, std::string>, std::array<std::uint16_t, 2>>;
}  // namespace label_test

TEST_CASE("variant labels: one label per alternative, in the schema and in text",
          "[schema_builder][variant][label]")
{
   using namespace label_test;
   namespace ir = psio::schema_types;
   // Rule 2: the type's name.
   static_assert(psio::variant_label<Generic, 0>() == "uint32[]");
   static_assert(psio::variant_label<Generic, 1>() == "Circle?");
   static_assert(psio::variant_label<Generic, 2>() == "tuple_uint8_string");
   static_assert(psio::variant_label<Generic, 3>() == "uint16[2]");
   // Rule 3: a type name two alternatives share, and a type with no name,
   // are no label: the position is.
   static_assert(psio::variant_label<Repeated, 0>() == "uint32");
   static_assert(psio::variant_label<Repeated, 1>() == "1");
   static_assert(psio::variant_label<Repeated, 2>() == "2");
   static_assert(psio::variant_label<Repeated, 3>() == "Circle");
   static_assert(psio::variant_label<Nameless, 0>() == "Circle");
   static_assert(psio::variant_label<Nameless, 1>() == "1");
   // Stated labels.
   static_assert(psio::variant_label<LabeledSchemaVariant, 0>() == "Code");
   static_assert(psio::variant_labels_ok<LabeledSchemaVariant>());
   static_assert(psio::variant_labels_ok<Repeated>());

   psio::SchemaBuilder builder;
   const auto repeated = builder.insert<Repeated>();
   const auto labeled  = builder.insert<LabeledSchemaVariant>();
   const auto schema   = std::move(builder).build();
   const auto& members = std::get<ir::Variant>(repeated.resolve(schema)->value).members;
   REQUIRE(members.size() == 4);
   REQUIRE(members[0].name == "uint32");
   REQUIRE(members[1].name == "1");
   REQUIRE(members[2].name == "2");
   REQUIRE(members[3].name == "Circle");
   for (const auto& member : members)
      REQUIRE(member.attributes.empty());

   // The typed text and the schema-driven text are keyed by the same label.
   const Repeated second{std::in_place_index<2>, "b"};
   REQUIRE(psio::encode(psio::ijson{}, second) == R"({"2":"b"})");
   REQUIRE(psio::decode<Repeated>(psio::ijson{}, std::string_view{R"({"2":"b"})"}) == second);
   REQUIRE(psio::decode<Repeated>(psio::ijson{}, std::string_view{R"({"Circle":{"radius":3}})"}) ==
           Repeated{Circle{3}});
   std::string text;
   bool        unknown = false;
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, repeated, psio::ijson{},
                                       psio::encode(psio::pssz{}, second), text, &unknown)
              .ok());
   REQUIRE(text == R"({"2":"b"})");

   // A stated label is the text key as well as the schema name.
   const LabeledSchemaVariant ready{true};
   REQUIRE(psio::encode(psio::ijson{}, ready) == R"({"Ready":true})");
   REQUIRE(psio::decode<LabeledSchemaVariant>(psio::ijson{}, std::string_view{R"({"Code":7})"}) ==
           LabeledSchemaVariant{std::uint16_t{7}});
   text.clear();
   REQUIRE(psio::pssz_ir::pssz_to_json(schema, labeled, psio::ijson{},
                                       psio::encode(psio::pssz{}, ready), text, &unknown)
              .ok());
   REQUIRE(text == R"({"Ready":true})");
   // The default label of the type is not read once labels are stated.
   LabeledSchemaVariant out;
   REQUIRE_FALSE(
      psio::decode_json_checked(psio::ijson{}, std::string_view{R"({"bool":true})"}, out).ok());
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

// ── Names: two types never become one node ────────────────────────────────
namespace sb_names
{
   namespace a
   {
      struct Config
      {
         std::uint32_t x = 0;
      };
      PSIO_REFLECT(Config, x)
   }
   namespace b
   {
      struct Config
      {
         std::string  name;
         std::uint8_t y = 0;
      };
      PSIO_REFLECT(Config, name, y)
   }
   struct Both
   {
      a::Config first;
      b::Config second;
   };
   PSIO_REFLECT(Both, first, second)
   struct Reversed
   {
      b::Config second;
      a::Config first;
   };
   PSIO_REFLECT(Reversed, second, first)

   template <typename T>
   struct WithBlock
   {
      std::uint32_t block = 0;
      T             data{};
      PSIO_REFLECT(WithBlock, block, data)
   };
   struct Sent
   {
      std::uint64_t amount = 0;
      PSIO_REFLECT(Sent, amount)
   };
   struct Events
   {
      WithBlock<Sent>         sent;
      WithBlock<std::uint8_t> tick;
      PSIO_REFLECT(Events, sent, tick)
   };

   namespace chosen
   {
      struct Config
      {
         std::int16_t z = 0;
      };
      PSIO_REFLECT(Config, z)
      struct Other
      {
         std::int16_t z = 0;
      };
      PSIO_REFLECT(Other, z)
   }
}  // namespace sb_names
template <>
struct psio::schema_type_name<sb_names::chosen::Config>
{
   static constexpr std::string_view value = "NodeConfig";
};
template <>
struct psio::schema_type_name<sb_names::chosen::Other>
{
   static constexpr std::string_view value = "NodeConfig";
};

TEST_CASE("schema builder: two types with one unqualified name are two nodes",
          "[schema][builder][names]")
{
   using namespace sb_names;
   namespace ir = psio::pssz_ir;
   psio::SchemaBuilder builder;
   builder.insert<Both>("Both");
   CHECK(builder.status().ok());
   const auto schema = builder.build();
   // Each is named as the compiler spells it; the plain name is nobody's.
   CHECK(schema.get("Config") == nullptr);
   REQUIRE(schema.get("sb_names::a::Config"));
   REQUIRE(schema.get("sb_names::b::Config"));
   CHECK(std::get<psio::schema_types::Object>(schema.get("sb_names::a::Config")->value).members.size() == 1);
   CHECK(std::get<psio::schema_types::Object>(schema.get("sb_names::b::Config")->value).members.size() == 2);
   const auto& both = std::get<psio::schema_types::Object>(schema.get("Both")->value);
   CHECK(std::get<psio::schema_types::Type>(both.members[0].type->value).type == "sb_names::a::Config");
   CHECK(std::get<psio::schema_types::Type>(both.members[1].type->value).type == "sb_names::b::Config");
   REQUIRE(ir::validate_schema(schema).ok());
   const auto collisions = builder.collisions();
   REQUIRE(collisions.size() == 1);
   CHECK(collisions[0].name == "Config");
   CHECK(collisions[0].types ==
         std::vector<std::string>{"sb_names::a::Config", "sb_names::b::Config"});

   // The schema describes the value (it did not: the second definition
   // replaced the first).
   const Both value{{7}, {"n", 1}};
   const auto bytes = psio::encode(psio::pssz{}, value);
   std::string text;
   REQUIRE(ir::pssz_to_json(schema, "Both", psio::ijson{}, std::span<const char>{bytes}, text).ok());
   CHECK(text == psio::encode(psio::ijson{}, value));
   CHECK(text == R"({"first":{"x":7},"second":{"name":"n","y":1}})");

   // The names do not depend on which of the two is met first.
   const auto reversed = psio::SchemaBuilder{}.insert<Reversed>("Reversed").build();
   CHECK(reversed.get("Config") == nullptr);
   CHECK(reversed.get("sb_names::a::Config"));
   CHECK(reversed.get("sb_names::b::Config"));
   CHECK(ir::equivalent(schema, "sb_names::a::Config", reversed, "sb_names::a::Config"));
   CHECK(ir::equivalent(schema, "sb_names::b::Config", reversed, "sb_names::b::Config"));

   // Without a collision a record keeps its unqualified name.
   psio::SchemaBuilder alone;
   const auto          one = alone.insert<b::Config>("Root").build();
   CHECK(one.get("Config"));
   CHECK(one.get("sb_names::b::Config") == nullptr);
   CHECK(alone.collisions().empty());
}

TEST_CASE("schema builder: two instantiations of one reflected template",
          "[schema][builder][names]")
{
   using namespace sb_names;
   namespace ir = psio::pssz_ir;
   const auto schema = psio::SchemaBuilder{}.insert<Events>("Events").build();
   REQUIRE(ir::validate_schema(schema).ok());
   CHECK(schema.get("WithBlock") == nullptr);
   const auto& events = std::get<psio::schema_types::Object>(schema.get("Events")->value);
   const auto  first  = std::get<psio::schema_types::Type>(events.members[0].type->value).type;
   const auto  second = std::get<psio::schema_types::Type>(events.members[1].type->value).type;
   CHECK(first != second);
   CHECK(first.starts_with("sb_names::WithBlock<"));
   CHECK(second.starts_with("sb_names::WithBlock<"));
   const Events value{{3, {9}}, {4, 5}};
   const auto   bytes = psio::encode(psio::pssz{}, value);
   std::string  text;
   REQUIRE(ir::pssz_to_json(schema, "Events", psio::ijson{}, std::span<const char>{bytes}, text)
              .ok());
   CHECK(text == psio::encode(psio::ijson{}, value));
}

TEST_CASE("schema builder: a root name and a record's name", "[schema][builder][names]")
{
   using namespace sb_names;
   namespace ir = psio::pssz_ir;
   // The root is asked for under the unqualified name another record has:
   // the record gives it up.
   {
      psio::SchemaBuilder builder;
      builder.insert<Both>("Both").insert<std::uint64_t>("Sent").insert<Sent>("Payment");
      CHECK(builder.status().ok());
      const auto schema = builder.build();
      REQUIRE(ir::validate_schema(schema).ok());
      CHECK(std::holds_alternative<psio::schema_types::Int>(schema.get("Sent")->resolve(schema)->value));
      CHECK(schema.get("sb_names::Sent"));
      CHECK(std::holds_alternative<psio::schema_types::Object>(schema.get("Payment")->resolve(schema)->value));
   }
   {
      psio::SchemaBuilder builder;
      builder.insert<Sent>("Payment").insert<std::uint64_t>("Sent");
      CHECK(builder.status().ok());
      const auto schema = builder.build();
      REQUIRE(ir::validate_schema(schema).ok());
      CHECK(std::holds_alternative<psio::schema_types::Int>(schema.get("Sent")->resolve(schema)->value));
      CHECK(std::get<psio::schema_types::Type>(schema.get("Payment")->value).type == "sb_names::Sent");
      CHECK(std::holds_alternative<psio::schema_types::Object>(schema.get("sb_names::Sent")->value));
   }
   // One name for two different roots is an error that names it.
   {
      psio::SchemaBuilder builder;
      builder.insert<std::uint64_t>("Id").insert<std::uint64_t>("Id");  // the same: fine
      CHECK(builder.status().ok());
      builder.insert<std::uint32_t>("Id");
      const auto st = builder.status();
      REQUIRE_FALSE(st.ok());
      CHECK(std::string{st.error().what} ==
            "psio::SchemaBuilder: insert<T>(\"Id\"): the name is already used in this schema for "
            "something else");
      CHECK_THROWS_AS(builder.build(), psio::codec_exception);
   }
}

TEST_CASE("schema builder: a chosen name", "[schema][builder][names]")
{
   using namespace sb_names;
   {
      psio::SchemaBuilder builder;
      const auto schema = builder.insert<chosen::Config>("Root").insert<a::Config>("A").build();
      CHECK(schema.get("NodeConfig"));
      // A chosen name takes no part in a collision: a::Config is alone.
      CHECK(schema.get("Config"));
      CHECK(builder.collisions().empty());
   }
   {
      psio::SchemaBuilder builder;
      builder.insert<chosen::Config>("Root");
      builder.insert<chosen::Other>("Other");
      const auto st = builder.status();
      REQUIRE_FALSE(st.ok());
      CHECK(std::string{st.error().what} ==
            "psio::SchemaBuilder: the name \"NodeConfig\" chosen for sb_names::chosen::Other "
            "(psio::schema_type_name) is already used in this schema (by "
            "sb_names::chosen::Config)");
   }
}

// ── What the numbered formats need of a member ───────────────────────────

namespace test_sb_numbered
{
   enum class Mode : std::int8_t
   {
      off  = 0,
      slow = -1,
      fast = 5
   };

   struct Plain
   {
      std::uint32_t id    = 0;
      std::int16_t  level = 0;
      Mode          mode  = Mode::off;
      double        ratio = 0;
      bool          on    = false;
      std::string   name;
      std::uint32_t old = 0;
      float         scale = 0;
   };
   PSIO_REFLECT(Plain, id, level, mode, ratio, on, name, old, scale)

   // The same record on the wire of PSSZ, with what protobuf, Cap'n Proto
   // and FlatBuffers read: field numbers, defaults, a retired member.
   struct Numbered
   {
      std::uint32_t id    = 0;
      std::int16_t  level = -7;
      Mode          mode  = Mode::slow;
      double        ratio = 0.5;
      bool          on    = true;
      std::string   name  = "not a scalar: no default";
      std::uint32_t old   = 0;
      float         scale = -0.0f;
   };
   PSIO_REFLECT(Numbered, attr(id, field<4>), level, mode, ratio, on, name,
                attr(old, field<9> | deprecated_spec{.version = "2.1"}), scale)

   struct Unit
   {
      std::variant<std::monostate, std::uint32_t> pick;
   };
   PSIO_REFLECT(Unit, pick)
}  // namespace test_sb_numbered

TEST_CASE("schema builder: field numbers, defaults and deprecation are member attributes",
          "[schema][builder][numbered]")
{
   namespace st = psio::schema_types;
   namespace ir = psio::pssz_ir;
   using test_sb_numbered::Numbered;
   using test_sb_numbered::Plain;

   psio::SchemaBuilder b;
   b.insert<Numbered>("Numbered");
   b.insert<Plain>("Plain");
   const psio::Schema schema = b.build();

   const auto& numbered = std::get<st::Object>(schema.get("Numbered")->value).members;
   using attrs = std::vector<st::Attribute>;
   CHECK(numbered[0].attributes == attrs{{"field", "4"}});
   CHECK(numbered[1].attributes == attrs{{"default", "-7"}});
   CHECK(numbered[2].attributes == attrs{{"default", "-1"}});   // an enum: its value
   CHECK(numbered[3].attributes == attrs{{"default", "0.5"}});
   CHECK(numbered[4].attributes == attrs{{"default", "true"}});
   CHECK(numbered[5].attributes.empty());
   CHECK(numbered[6].attributes == attrs{{"field", "9"}, {"deprecated", "2.1"}});
   CHECK(numbered[7].attributes == attrs{{"default", "-0"}});
   // Nothing is written for what the defaults already say.
   for (const auto& m : std::get<st::Object>(schema.get("Plain")->value).members)
      CHECK(m.attributes.empty());

   // No PSSZ byte depends on them: the schema-driven codec accepts the
   // attributes and reads the same bytes; the two records are one type.
   Numbered value;
   value.id    = 7;
   value.level = 300;
   value.name  = "n";
   const auto bytes = psio::encode(psio::pssz{}, value);
   CHECK(ir::validate_pssz(schema, st::AnyType{st::Type{"Numbered"}}, std::span<const char>{bytes}).ok());
   CHECK(ir::validate_pssz(schema, st::AnyType{st::Type{"Plain"}}, std::span<const char>{bytes}).ok());
   CHECK(ir::equivalent(schema, "Numbered", schema, "Plain"));
   CHECK(ir::evolves_from(schema, "Plain", schema, "Numbered"));
   CHECK(ir::evolves_from(schema, "Numbered", schema, "Plain"));

   // They mean something on a record member only.
   psio::Schema misplaced = schema;
   std::get<st::Object>(misplaced.types[0].type->value).attributes.push_back({"default", "1"});
   CHECK_FALSE(ir::validate_pssz(misplaced, st::AnyType{st::Type{"Numbered"}},
                                 std::span<const char>{bytes})
                  .ok());

   // std::monostate is the unit type.
   psio::SchemaBuilder u;
   u.insert<test_sb_numbered::Unit>("Unit");
   const psio::Schema unit = u.build();
   const auto&        pick = std::get<st::Object>(unit.get("Unit")->value).members[0];
   const auto*        v    = std::get_if<st::Variant>(&pick.type->resolve(unit)->value);
   REQUIRE(v != nullptr);
   const auto* none = v->members[0].type->resolve(unit);
   REQUIRE(none != nullptr);
   CHECK(*none == st::AnyType{st::Tuple{}});
}
