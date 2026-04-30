// Tests for psio/capnp_parser.hpp — Cap'n Proto schema text → Schema IR.

#include <psio/capnp_parser.hpp>
#include <psio/emit_capnp.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

using psio::AnyType;
using psio::Schema;
using psio::schema_types::Custom;
using psio::schema_types::Int;
using psio::schema_types::List;
using psio::schema_types::Object;
using psio::schema_types::Resource;
using psio::schema_types::Tuple;
using psio::schema_types::Type;
using psio::schema_types::Variant;

// ─── File header + struct ────────────────────────────────────────────

TEST_CASE("parse_capnp: file id + struct with primitives", "[capnp_parser]")
{
   const std::string_view src =
      "@0xdeadbeef00000001;\n"
      "\n"
      "struct Point {\n"
      "  x @0 :Int32;\n"
      "  y @1 :Int32;\n"
      "}\n";

   auto s = psio::parse_capnp(src);

   bool found_id = false;
   for (const auto& a : s.package.attributes)
      if (a.name == "capnp_id" && a.value &&
          *a.value == "@0xdeadbeef00000001")
         found_id = true;
   CHECK(found_id);

   const auto* point = s.get("point");
   REQUIRE(point != nullptr);
   const auto* obj = std::get_if<Object>(&point->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 2);
   CHECK(obj->members[0].name == "x");
   CHECK(obj->members[1].name == "y");

   // Ordinal carrier survives.
   bool has_ord_0 = false;
   for (const auto& a : obj->members[0].attributes)
      if (a.name == "ordinal" && a.value && *a.value == "0")
         has_ord_0 = true;
   CHECK(has_ord_0);
}

// ─── Variant via struct-with-anonymous-union ─────────────────────────

TEST_CASE("parse_capnp: struct with anonymous union → Variant",
          "[capnp_parser]")
{
   const std::string_view src =
      "struct Status {\n"
      "  union {\n"
      "    ok @0 :Void;\n"
      "    err @1 :UInt32;\n"
      "  }\n"
      "}\n";

   auto s = psio::parse_capnp(src);

   const auto* status = s.get("status");
   REQUIRE(status != nullptr);
   const auto* var = std::get_if<Variant>(&status->value);
   REQUIRE(var != nullptr);
   REQUIRE(var->members.size() == 2);
   CHECK(var->members[0].name == "ok");
   CHECK(var->members[1].name == "err");

   // ok is Tuple{} (Void), err is UInt32.
   auto* ok_tup = std::get_if<Tuple>(&var->members[0].type->value);
   REQUIRE(ok_tup != nullptr);
   CHECK(ok_tup->members.empty());
}

TEST_CASE("parse_capnp: enum → Variant of empty cases", "[capnp_parser]")
{
   const std::string_view src =
      "enum Color {\n"
      "  red @0;\n"
      "  green @1;\n"
      "  blue @2;\n"
      "}\n";

   auto s = psio::parse_capnp(src);

   const auto* color = s.get("color");
   REQUIRE(color != nullptr);
   const auto* var = std::get_if<Variant>(&color->value);
   REQUIRE(var != nullptr);
   REQUIRE(var->members.size() == 3);
   CHECK(var->members[0].name == "red");
   CHECK(std::get_if<Tuple>(&var->members[0].type->value) != nullptr);
}

// ─── Interface ───────────────────────────────────────────────────────

TEST_CASE("parse_capnp: interface → Resource with methods",
          "[capnp_parser]")
{
   const std::string_view src =
      "interface Greeter {\n"
      "  greet @0 (name :Text) -> (result :Text);\n"
      "  ping @1 ();\n"
      "}\n";

   auto s = psio::parse_capnp(src);

   const auto* greeter = s.get("greeter");
   REQUIRE(greeter != nullptr);
   const auto* res = std::get_if<Resource>(&greeter->value);
   REQUIRE(res != nullptr);
   REQUIRE(res->methods.size() == 2);
   CHECK(res->methods[0].name == "greet");
   REQUIRE(res->methods[0].params.size() == 1);
   CHECK(res->methods[0].params[0].name == "name");
   CHECK(res->methods[0].result.has_value());
   CHECK(res->methods[1].name == "ping");
   CHECK_FALSE(res->methods[1].result.has_value());
}

// ─── List / Text / Data type spellings ───────────────────────────────

TEST_CASE("parse_capnp: List/Text/Data render to expected IR shapes",
          "[capnp_parser]")
{
   const std::string_view src =
      "struct Blob {\n"
      "  name @0 :Text;\n"
      "  payload @1 :Data;\n"
      "  ids @2 :List(UInt32);\n"
      "}\n";

   auto s = psio::parse_capnp(src);

   const auto* blob = s.get("blob");
   REQUIRE(blob != nullptr);
   const auto* obj = std::get_if<Object>(&blob->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 3);

   // name: Custom{List<u8>, "string"}
   auto* nc = std::get_if<Custom>(&obj->members[0].type->value);
   REQUIRE(nc != nullptr);
   CHECK(nc->id == "string");

   // payload: Custom{List<u8>, "hex"}
   auto* pc = std::get_if<Custom>(&obj->members[1].type->value);
   REQUIRE(pc != nullptr);
   CHECK(pc->id == "hex");

   // ids: List<u32>
   auto* lst = std::get_if<List>(&obj->members[2].type->value);
   REQUIRE(lst != nullptr);
   auto* lst_inner = std::get_if<Int>(&lst->type->value);
   REQUIRE(lst_inner != nullptr);
   CHECK(lst_inner->bits == 32);
}

// ─── Round-trip: emit_capnp → parse_capnp ────────────────────────────

TEST_CASE("parse_capnp: round-trips emit_capnp output", "[capnp_parser][rt]")
{
   // Build a Schema, emit, parse back, assert structural equality on
   // the named types.
   Schema s;
   s.insert("point",
            AnyType{Object{.members = {
                              psio::Member{.name = "x",
                                           .type = psio::schema_types::Box<AnyType>{
                                              Int{32, true}}},
                              psio::Member{.name = "y",
                                           .type = psio::schema_types::Box<AnyType>{
                                              Int{32, true}}}}}});

   auto text   = psio::emit_capnp(s);
   auto parsed = psio::parse_capnp(text);

   const auto* p1 = s.get("point");
   const auto* p2 = parsed.get("point");
   REQUIRE(p1 != nullptr);
   REQUIRE(p2 != nullptr);

   const auto* o1 = std::get_if<Object>(&p1->value);
   const auto* o2 = std::get_if<Object>(&p2->value);
   REQUIRE(o1 != nullptr);
   REQUIRE(o2 != nullptr);
   REQUIRE(o1->members.size() == o2->members.size());
   for (std::size_t i = 0; i < o1->members.size(); ++i)
      CHECK(o1->members[i].name == o2->members[i].name);
}

// ─── Errors ──────────────────────────────────────────────────────────

TEST_CASE("parse_capnp: malformed input throws with line/column",
          "[capnp_parser]")
{
   const std::string_view bad =
      "struct Point {\n"
      "  x @0 \n";  // missing :Type;

   bool threw = false;
   try
   {
      psio::parse_capnp(bad);
   }
   catch (const psio::capnp_parse_error& e)
   {
      threw = true;
      CHECK(e.line >= 2);
   }
   CHECK(threw);
}
