// Tests for psio/fbs_parser.hpp — FlatBuffers schema text → Schema IR.

#include <psio/fbs_parser.hpp>
#include <psio/emit_fbs.hpp>

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
using psio::schema_types::Struct;
using psio::schema_types::Tuple;
using psio::schema_types::Variant;

// ─── namespace + table ───────────────────────────────────────────────

TEST_CASE("parse_fbs: namespace + table with primitives", "[fbs_parser]")
{
   const std::string_view src =
      "namespace wasi.io;\n"
      "\n"
      "table Point {\n"
      "  x:int;\n"
      "  y:int;\n"
      "}\n";

   auto s = psio::parse_fbs(src);

   CHECK(s.package.name == "wasi:io");

   const auto* point = s.get("point");
   REQUIRE(point != nullptr);
   const auto* obj = std::get_if<Object>(&point->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 2);
   CHECK(obj->members[0].name == "x");
   CHECK(obj->members[1].name == "y");
}

// ─── primitive width mapping ─────────────────────────────────────────

TEST_CASE("parse_fbs: int / uint width spellings", "[fbs_parser]")
{
   const std::string_view src =
      "table Sizes {\n"
      "  a:byte; b:ubyte;\n"
      "  c:short; d:ushort;\n"
      "  e:int; f:uint;\n"
      "  g:long; h:ulong;\n"
      "  i:float; j:double;\n"
      "}\n";

   auto s   = psio::parse_fbs(src);
   const auto* sz = s.get("sizes");
   REQUIRE(sz != nullptr);
   const auto* obj = std::get_if<Object>(&sz->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 10);

   auto bits = [&](std::size_t i) {
      const auto* x = std::get_if<Int>(&obj->members[i].type->value);
      REQUIRE(x != nullptr);
      return std::pair{x->bits, x->isSigned};
   };

   CHECK(bits(0) == std::pair{8u, true});
   CHECK(bits(1) == std::pair{8u, false});
   CHECK(bits(2) == std::pair{16u, true});
   CHECK(bits(3) == std::pair{16u, false});
   CHECK(bits(4) == std::pair{32u, true});
   CHECK(bits(5) == std::pair{32u, false});
   CHECK(bits(6) == std::pair{64u, true});
   CHECK(bits(7) == std::pair{64u, false});
}

// ─── string + list + raw bytes ───────────────────────────────────────

TEST_CASE("parse_fbs: string / [T] / [ubyte]", "[fbs_parser]")
{
   const std::string_view src =
      "table Blob {\n"
      "  name:string;\n"
      "  payload:[ubyte];\n"
      "  ids:[uint];\n"
      "}\n";

   auto s = psio::parse_fbs(src);
   const auto* blob = s.get("blob");
   REQUIRE(blob != nullptr);
   const auto* obj = std::get_if<Object>(&blob->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 3);

   // name: Custom{List<u8>, "string"}
   auto* nc = std::get_if<Custom>(&obj->members[0].type->value);
   REQUIRE(nc != nullptr);
   CHECK(nc->id == "string");

   // payload: List<u8>  — parser returns the bare list shape,
   // emit_fbs's "hex" Custom marker is one-way (lossy on round-trip).
   auto* pl = std::get_if<List>(&obj->members[1].type->value);
   REQUIRE(pl != nullptr);
   auto* pl_inner = std::get_if<Int>(&pl->type->value);
   REQUIRE(pl_inner != nullptr);
   CHECK(pl_inner->bits == 8);

   // ids: List<u32>
   auto* lst = std::get_if<List>(&obj->members[2].type->value);
   REQUIRE(lst != nullptr);
   auto* lst_inner = std::get_if<Int>(&lst->type->value);
   REQUIRE(lst_inner != nullptr);
   CHECK(lst_inner->bits == 32);
}

// ─── enum → Variant of empty cases ───────────────────────────────────

TEST_CASE("parse_fbs: enum → Variant of empty cases", "[fbs_parser]")
{
   const std::string_view src =
      "enum Color : ubyte {\n"
      "  red,\n"
      "  green,\n"
      "  blue\n"
      "}\n";

   auto s = psio::parse_fbs(src);
   const auto* color = s.get("color");
   REQUIRE(color != nullptr);
   const auto* var = std::get_if<Variant>(&color->value);
   REQUIRE(var != nullptr);
   REQUIRE(var->members.size() == 3);
   CHECK(var->members[0].name == "red");
   CHECK(std::get_if<Tuple>(&var->members[0].type->value) != nullptr);
}

// ─── union+wrapper folded into a single Variant ──────────────────────

TEST_CASE("parse_fbs: union + wrapper-table collapse to Variant",
          "[fbs_parser]")
{
   // Mirrors what emit_fbs produces for a payload-bearing Variant.
   const std::string_view src =
      "union StatusUnion {\n"
      "  Ok,\n"
      "  Err\n"
      "}\n"
      "table Ok {}\n"
      "table Err { value:uint; }\n"
      "table Status {\n"
      "  value:StatusUnion;\n"
      "}\n";

   auto s = psio::parse_fbs(src);

   const auto* status = s.get("status");
   REQUIRE(status != nullptr);
   const auto* var = std::get_if<Variant>(&status->value);
   REQUIRE(var != nullptr);
   REQUIRE(var->members.size() == 2);
   CHECK(var->members[0].name == "ok");
   CHECK(var->members[1].name == "err");
}

// ─── field id round-trips through fbs_id attribute ───────────────────

TEST_CASE("parse_fbs: field (id: N) → Member.attributes['fbs_id']",
          "[fbs_parser]")
{
   const std::string_view src =
      "table Evolved {\n"
      "  old_field:uint (id: 3);\n"
      "  new_field:uint (id: 7);\n"
      "}\n";

   auto s = psio::parse_fbs(src);
   const auto* evolved = s.get("evolved");
   REQUIRE(evolved != nullptr);
   const auto* obj = std::get_if<Object>(&evolved->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 2);

   auto find_attr = [](const auto& attrs, std::string_view name)
      -> const std::optional<std::string>* {
      for (const auto& a : attrs)
         if (a.name == name)
            return &a.value;
      return nullptr;
   };
   const auto* a0 = find_attr(obj->members[0].attributes, "fbs_id");
   const auto* a1 = find_attr(obj->members[1].attributes, "fbs_id");
   REQUIRE(a0 != nullptr);
   REQUIRE(a1 != nullptr);
   REQUIRE(a0->has_value());
   REQUIRE(a1->has_value());
   CHECK(**a0 == "3");
   CHECK(**a1 == "7");
}

// ─── round-trip: emit_fbs → parse_fbs ────────────────────────────────

TEST_CASE("parse_fbs: round-trips emit_fbs output for simple records",
          "[fbs_parser][rt]")
{
   Schema s;
   s.insert(
      "point",
      AnyType{Object{
         .members = {
            psio::Member{.name = "x",
                         .type = psio::schema_types::Box<AnyType>{
                            Int{32, true}}},
            psio::Member{.name = "y",
                         .type = psio::schema_types::Box<AnyType>{
                            Int{32, true}}}}}});

   auto text   = psio::emit_fbs(s);
   auto parsed = psio::parse_fbs(text);

   const auto* p = parsed.get("point");
   REQUIRE(p != nullptr);
   const auto* obj = std::get_if<Object>(&p->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 2);
   CHECK(obj->members[0].name == "x");
   CHECK(obj->members[1].name == "y");
}

// ─── error reporting ────────────────────────────────────────────────

TEST_CASE("parse_fbs: malformed input throws with line/column",
          "[fbs_parser]")
{
   const std::string_view bad =
      "table Point {\n"
      "  x:int\n";  // missing semicolon

   bool threw = false;
   try
   {
      psio::parse_fbs(bad);
   }
   catch (const psio::fbs_parse_error& e)
   {
      threw = true;
      CHECK(e.line >= 2);
   }
   CHECK(threw);
}
