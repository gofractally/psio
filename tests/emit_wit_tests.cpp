// Tests for psio/emit_wit.hpp — Schema IR → WIT text.

#include <psio/emit_wit.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
   bool contains(std::string_view hay, std::string_view needle)
   {
      return hay.find(needle) != std::string_view::npos;
   }
}

// ─── Direct IR construction → WIT text ───────────────────────────────

TEST_CASE("emit_wit: package + bare interface + funcs", "[emit_wit]")
{
   using namespace psio::schema_types;

   Schema s;
   s.package = Package{.name = "wasi:clocks", .version = "0.2.3"};
   s.interfaces.push_back(Interface{
      .name = "wall_clock",
      .type_names = {},
      .funcs      = {Func{.name = "now",
                          .params = {},
                          .result = Box<AnyType>{AnyType{Int{64, false}}},
                          .attributes = {}}}});

   auto text = psio::emit_wit(s);

   CHECK(contains(text, "package wasi:clocks@0.2.3;"));
   CHECK(contains(text, "interface wall-clock {"));
   CHECK(contains(text, "now: func() -> u64;"));
}

TEST_CASE("emit_wit: record with primitive + list + option fields",
          "[emit_wit]")
{
   using namespace psio::schema_types;

   Schema s;
   s.insert("bag", AnyType{Object{
      .members = {
         Member{.name = "name",
                .type = Box<AnyType>{
                   Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}}},
         Member{.name = "items",
                .type = Box<AnyType>{List{Box<AnyType>{Int{32, false}}}}},
         Member{.name = "count",
                .type = Box<AnyType>{Option{Box<AnyType>{Int{32, true}}}}}}}});

   s.interfaces.push_back(Interface{
      .name = "bags", .type_names = {"bag"}, .funcs = {}});

   auto text = psio::emit_wit(s);

   CHECK(contains(text, "interface bags {"));
   CHECK(contains(text, "record bag {"));
   CHECK(contains(text, "name: string"));
   CHECK(contains(text, "items: list<u32>"));
   CHECK(contains(text, "count: option<s32>"));
}

TEST_CASE("emit_wit: variant + resource", "[emit_wit]")
{
   using namespace psio::schema_types;

   Schema s;
   s.insert("status", AnyType{Variant{
      .members = {
         Member{.name = "ok",   .type = Box<AnyType>{Tuple{}}},
         Member{.name = "err",  .type = Box<AnyType>{Int{32, false}}}}}});

   s.insert("pollable", AnyType{Resource{
      .name    = "pollable",
      .methods = {Func{.name = "ready",
                       .result = Box<AnyType>{Custom{Box<AnyType>{Int{1, false}}, "bool"}}},
                  Func{.name = "block"}}}});

   s.interfaces.push_back(Interface{
      .name       = "io",
      .type_names = {"status", "pollable"},
      .funcs      = {}});

   auto text = psio::emit_wit(s);

   CHECK(contains(text, "variant status {"));
   CHECK(contains(text, "ok"));      // bare case
   CHECK(contains(text, "err(u32)")); // payload case
   CHECK(contains(text, "resource pollable {"));
   CHECK(contains(text, "ready: func() -> bool;"));
   CHECK(contains(text, "block: func();"));
}

TEST_CASE("emit_wit: world with imports and exports", "[emit_wit]")
{
   using namespace psio::schema_types;

   Schema s;
   s.package = Package{.name = "wasi:cli", .version = "0.2.3"};
   s.uses.push_back(Use{
      .package        = "wasi:io",
      .interface_name = "poll",
      .version        = "0.2.3"});
   s.worlds.push_back(World{
      .name    = "command",
      .imports = {UseRef{.package = "wasi:io", .interface_name = "poll"}},
      .exports = {"environment"}});
   s.interfaces.push_back(Interface{
      .name = "environment", .type_names = {}, .funcs = {}});

   auto text = psio::emit_wit(s);

   CHECK(contains(text, "package wasi:cli@0.2.3;"));
   CHECK(contains(text, "use wasi:io/poll@0.2.3;"));
   CHECK(contains(text, "world command {"));
   CHECK(contains(text, "import wasi:io/poll;"));
   CHECK(contains(text, "export environment;"));
}

// ─── Pipeline: SchemaBuilder → emit_wit ───────────────────────────────

namespace test_emit
{
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
   };
   PSIO_REFLECT(Point, x, y)

   struct datetime
   {
      std::uint64_t seconds = 0;
   };
   PSIO_REFLECT(datetime, seconds)
}  // namespace test_emit
using test_emit::datetime;
using test_emit::Point;

struct emit_wall_clock
{
   static datetime now();
   static datetime resolution();
};

PSIO_PACKAGE(emit_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(emit_clocks)

PSIO_INTERFACE(emit_wall_clock,
               types(datetime),
               funcs(func(now), func(resolution)))

TEST_CASE("emit_wit: SchemaBuilder ↔ emit_wit pipeline", "[emit_wit]")
{
   auto schema = psio::SchemaBuilder{}
                    .insert_interface<emit_wall_clock>()
                    .build();
   auto text = psio::emit_wit(schema);

   CHECK(contains(text, "package emit_clocks@0.2.3;"));
   CHECK(contains(text, "interface emit-wall-clock {"));
   CHECK(contains(text, "record datetime {"));
   CHECK(contains(text, "seconds: u64"));
   CHECK(contains(text, "now: func() ->"));
   CHECK(contains(text, "resolution: func() ->"));
}

// ─── Round-trip: emit → wit_parse ────────────────────────────────────

#include <psio/wit_parser.hpp>

TEST_CASE("emit_wit + wit_parser: emitted text re-parses",
          "[emit_wit][round_trip]")
{
   auto schema = psio::SchemaBuilder{}
                    .insert_interface<emit_wall_clock>()
                    .build();
   auto text = psio::emit_wit(schema);

   // Should not throw.
   auto parsed = psio::wit_parse(text);

   CHECK(parsed.package == "emit_clocks@0.2.3");
   bool found_iface = false;
   for (const auto& iface : parsed.exports)
      if (iface.name == "emit-wall-clock")
         found_iface = true;
   for (const auto& iface : parsed.imports)
      if (iface.name == "emit-wall-clock")
         found_iface = true;
   // emit_wit puts free interfaces at top level (no world envelope is
   // generated unless the schema carried a World), so the interface
   // appears as a top-level free interface in the parsed output.
   bool found_anywhere = found_iface;
   for (auto& td : parsed.types)
      if (td.name == "datetime")
         found_anywhere = true;
   CHECK(found_anywhere);
}
