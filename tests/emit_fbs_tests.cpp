// Tests for psio/emit_fbs.hpp — Schema IR → FlatBuffers schema text.

#include <psio/emit_fbs.hpp>
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

using psio::AnyType;
using psio::Func;
using psio::Interface;
using psio::Member;
using psio::Package;
using psio::Schema;
using psio::schema_types::Box;
using psio::schema_types::Custom;
using psio::schema_types::Int;
using psio::schema_types::List;
using psio::schema_types::Object;
using psio::schema_types::Tuple;
using psio::schema_types::Type;
using psio::schema_types::Variant;

// ─── namespace + table ───────────────────────────────────────────────

TEST_CASE("emit_fbs: namespace + table with primitives", "[emit_fbs]")
{
   Schema s;
   s.package = Package{.name = "wasi:io", .version = "0.2.3"};
   s.insert("point",
            AnyType{Object{.members = {
                              Member{.name = "x",
                                     .type = Box<AnyType>{Int{32, true}}},
                              Member{.name = "y",
                                     .type = Box<AnyType>{Int{32, true}}}}}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "namespace wasi.io;"));
   CHECK(contains(text, "table Point {"));
   CHECK(contains(text, "x:int;"));
   CHECK(contains(text, "y:int;"));
}

// ─── primitive width mapping ─────────────────────────────────────────

TEST_CASE("emit_fbs: int / uint width spellings", "[emit_fbs]")
{
   Schema s;
   s.insert(
      "all_ints",
      AnyType{Object{
         .members = {
            Member{.name = "a", .type = Box<AnyType>{Int{8, true}}},
            Member{.name = "b", .type = Box<AnyType>{Int{8, false}}},
            Member{.name = "c", .type = Box<AnyType>{Int{16, true}}},
            Member{.name = "d", .type = Box<AnyType>{Int{16, false}}},
            Member{.name = "e", .type = Box<AnyType>{Int{32, true}}},
            Member{.name = "f", .type = Box<AnyType>{Int{32, false}}},
            Member{.name = "g", .type = Box<AnyType>{Int{64, true}}},
            Member{.name = "h", .type = Box<AnyType>{Int{64, false}}}}}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "a:byte;"));
   CHECK(contains(text, "b:ubyte;"));
   CHECK(contains(text, "c:short;"));
   CHECK(contains(text, "d:ushort;"));
   CHECK(contains(text, "e:int;"));
   CHECK(contains(text, "f:uint;"));
   CHECK(contains(text, "g:long;"));
   CHECK(contains(text, "h:ulong;"));
}

// ─── string + list + bytes ───────────────────────────────────────────

TEST_CASE("emit_fbs: string + list + raw bytes", "[emit_fbs]")
{
   Schema s;
   s.insert(
      "blob",
      AnyType{Object{
         .members = {
            Member{.name = "name",
                   .type = Box<AnyType>{Custom{
                      Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                      "string"}}},
            Member{.name = "payload",
                   .type = Box<AnyType>{Custom{
                      Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                      "hex"}}},
            Member{.name = "ids",
                   .type = Box<AnyType>{
                      List{Box<AnyType>{Int{32, false}}}}}}}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "name:string;"));
   CHECK(contains(text, "payload:[ubyte];"));
   CHECK(contains(text, "ids:[uint];"));
}

// ─── enum (Variant of empty cases) ───────────────────────────────────

TEST_CASE("emit_fbs: Variant of empty cases emits as enum",
          "[emit_fbs]")
{
   Schema s;
   s.insert("color",
            AnyType{Variant{.members = {
                               Member{.name = "red",
                                      .type = Box<AnyType>{Tuple{}}},
                               Member{.name = "green",
                                      .type = Box<AnyType>{Tuple{}}},
                               Member{.name = "blue",
                                      .type = Box<AnyType>{Tuple{}}}}}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "enum Color : ubyte {"));
   CHECK(contains(text, "red"));
   CHECK(contains(text, "green"));
   CHECK(contains(text, "blue"));
}

// ─── union (Variant with payload cases) ──────────────────────────────

TEST_CASE("emit_fbs: Variant with payloads emits union + wrapper table",
          "[emit_fbs]")
{
   Schema s;
   s.insert("status",
            AnyType{Variant{.members = {
                               Member{.name = "ok",
                                      .type = Box<AnyType>{Tuple{}}},
                               Member{.name = "err",
                                      .type = Box<AnyType>{Int{32, false}}}}}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "union StatusUnion {"));
   CHECK(contains(text, "Ok"));
   CHECK(contains(text, "Err"));
   // Wrapper tables for the cases.
   CHECK(contains(text, "table Ok {}"));   // bare case
   CHECK(contains(text, "table Err {"));    // payload case
   // Outer table that hosts the union.
   CHECK(contains(text, "table Status {"));
   CHECK(contains(text, "value:StatusUnion;"));
}

// ─── field id carrier ────────────────────────────────────────────────

TEST_CASE("emit_fbs: explicit fbs_id attribute renders as (id: N)",
          "[emit_fbs]")
{
   Schema s;
   s.insert(
      "evolved",
      AnyType{Object{
         .members = {
            Member{.name       = "old_field",
                   .type       = Box<AnyType>{Int{32, false}},
                   .attributes = {{"fbs_id", std::string{"3"}}}},
            Member{.name       = "new_field",
                   .type       = Box<AnyType>{Int{32, false}},
                   .attributes = {{"fbs_id", std::string{"7"}}}}}}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "old_field:uint (id: 3);"));
   CHECK(contains(text, "new_field:uint (id: 7);"));
}

// ─── root_type ───────────────────────────────────────────────────────

TEST_CASE("emit_fbs: emits root_type from interface's first export",
          "[emit_fbs]")
{
   Schema s;
   s.insert("entry", AnyType{Object{}});
   s.interfaces.push_back(Interface{
      .name = "api", .type_names = {"entry"}, .funcs = {}});

   auto text = psio::emit_fbs(s);

   CHECK(contains(text, "root_type Entry;"));
}

// ─── SchemaBuilder pipeline ──────────────────────────────────────────

namespace test_fbs_emit
{
   struct datetime
   {
      std::uint64_t seconds = 0;
   };
   PSIO_REFLECT(datetime, seconds)
}  // namespace test_fbs_emit
using test_fbs_emit::datetime;

struct fbs_wall_clock
{
   static datetime now();
};

PSIO_PACKAGE(fbs_clocks_pkg, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(fbs_clocks_pkg)

PSIO_INTERFACE(fbs_wall_clock, types(datetime), funcs(func(now)))

TEST_CASE("emit_fbs: SchemaBuilder pipeline produces well-formed text",
          "[emit_fbs][pipeline]")
{
   auto schema = psio::SchemaBuilder{}
                    .insert_interface<fbs_wall_clock>()
                    .build();
   auto text = psio::emit_fbs(schema);

   CHECK(contains(text, "namespace fbs_clocks_pkg;"));
   CHECK(contains(text, "table Datetime {"));
   CHECK(contains(text, "seconds:ulong;"));
   CHECK(contains(text, "root_type Datetime;"));
}
