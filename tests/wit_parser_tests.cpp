// Tests for psio/wit_parser.hpp — recursive-descent WIT text parser.

#include <psio/wit_parser.hpp>
#include <psio/wit_gen.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// ─── Direct parser tests ─────────────────────────────────────────────

TEST_CASE("wit_parse: package + interface with primitive func",
          "[wit_parser]")
{
   const std::string_view src =
      "package my:pkg@1.0.0;\n"
      "interface greeter {\n"
      "  greet: func(name: string) -> string;\n"
      "}\n"
      "world hello {\n"
      "  export greeter;\n"
      "}\n";

   auto w = psio::wit_parse(src);

   CHECK(w.package == "my:pkg@1.0.0");
   CHECK(w.name == "hello");

   // Free top-level `interface greeter { … }` lands in exports as a
   // populated entry; the world's `export greeter;` adds a bare-name
   // reference to the same exports vector.  v1's parser elides no
   // distinction; we pin the current behavior so a future cleanup is
   // intentional and reviewable.
   bool found_greeter = false;
   for (const auto& iface : w.exports)
      if (iface.name == "greeter")
         found_greeter = true;
   CHECK(found_greeter);

   REQUIRE(w.funcs.size() == 1);
   CHECK(w.funcs[0].name == "greet");
   REQUIRE(w.funcs[0].params.size() == 1);
   CHECK(w.funcs[0].params[0].name == "name");
   CHECK(w.funcs[0].params[0].type_idx ==
         psio::wit_prim_idx(psio::wit_prim::string_));
   REQUIRE(w.funcs[0].results.size() == 1);
   CHECK(w.funcs[0].results[0].type_idx ==
         psio::wit_prim_idx(psio::wit_prim::string_));
}

TEST_CASE("wit_parse: record + list/option types", "[wit_parser]")
{
   const std::string_view src =
      "package test:pkg@1.0.0;\n"
      "interface api {\n"
      "  record point {\n"
      "    x: u32,\n"
      "    y: u32,\n"
      "  }\n"
      "  fetch: func(name: string) -> list<u8>;\n"
      "  lookup: func(key: string) -> option<u32>;\n"
      "}\n";

   auto w = psio::wit_parse(src);

   // Record was registered.
   bool found_point = false;
   for (const auto& td : w.types)
      if (td.name == "point" &&
          td.kind == static_cast<std::uint8_t>(psio::wit_type_kind::record_))
      {
         found_point = true;
         REQUIRE(td.fields.size() == 2);
         CHECK(td.fields[0].name == "x");
         CHECK(td.fields[1].name == "y");
      }
   CHECK(found_point);

   // list<u8> + option<u32> appear as anonymous types.
   bool found_list_u8 = false;
   bool found_opt_u32 = false;
   for (const auto& td : w.types)
   {
      if (td.kind == static_cast<std::uint8_t>(psio::wit_type_kind::list_) &&
          td.element_type_idx == psio::wit_prim_idx(psio::wit_prim::u8))
         found_list_u8 = true;
      if (td.kind == static_cast<std::uint8_t>(psio::wit_type_kind::option_) &&
          td.element_type_idx == psio::wit_prim_idx(psio::wit_prim::u32))
         found_opt_u32 = true;
   }
   CHECK(found_list_u8);
   CHECK(found_opt_u32);
}

// ─── Round-trip: gen → text → parse → equivalent IR ──────────────────

namespace test_witparse
{
   struct datetime
   {
      std::uint64_t seconds     = 0;
      std::uint32_t nanoseconds = 0;
   };
   PSIO_REFLECT(datetime, seconds, nanoseconds)
}  // namespace test_witparse
using test_witparse::datetime;

struct witparse_wall
{
   static datetime now();
   static datetime resolution();
};

PSIO_PACKAGE(witparse_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(witparse_clocks)

PSIO_INTERFACE(witparse_wall,
               types(datetime),
               funcs(func(now), func(resolution)))

TEST_CASE("wit_parse: round-trips wit_gen output", "[wit_parser]")
{
   auto generated = psio::generate_wit_text<witparse_wall>(
      "wasi", "clocks", "0.2.3");

   auto parsed = psio::wit_parse(generated);

   CHECK(parsed.package == "wasi:clocks@0.2.3");
   CHECK(parsed.name == "witparse-wall");
   // wit_gen emits one free interface + one bare-export; parser surfaces both.
   bool found_witparse_wall = false;
   for (const auto& iface : parsed.exports)
      if (iface.name == "witparse-wall")
         found_witparse_wall = true;
   CHECK(found_witparse_wall);

   // datetime record survives the round-trip.
   bool found_datetime = false;
   for (const auto& td : parsed.types)
      if (td.name == "datetime" && td.fields.size() == 2 &&
          td.fields[0].name == "seconds" &&
          td.fields[1].name == "nanoseconds")
         found_datetime = true;
   CHECK(found_datetime);

   // now / resolution funcs survive.
   bool found_now = false, found_res = false;
   for (const auto& f : parsed.funcs)
   {
      if (f.name == "now")        found_now = true;
      if (f.name == "resolution") found_res = true;
   }
   CHECK(found_now);
   CHECK(found_res);
}

// ─── Error reporting ─────────────────────────────────────────────────

TEST_CASE("wit_parse: reports parse errors with line/column",
          "[wit_parser]")
{
   const std::string_view bad =
      "package my:pkg@1.0.0;\n"
      "interface greeter {\n"
      "  greet: func(\n";  // unterminated

   bool threw = false;
   try
   {
      psio::wit_parse(bad);
   }
   catch (const psio::wit_parse_error& e)
   {
      threw = true;
      CHECK(e.line >= 3);
   }
   CHECK(threw);
}
