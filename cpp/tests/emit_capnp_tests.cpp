// Tests for psio/emit_capnp.hpp — Schema IR → Cap'n Proto schema text.

#include <psio/emit_capnp.hpp>
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
using psio::Resource;
using psio::Schema;
using psio::schema_types::Box;
using psio::schema_types::Custom;
using psio::schema_types::Int;
using psio::schema_types::List;
using psio::schema_types::Object;
using psio::schema_types::Tuple;
using psio::schema_types::Type;
using psio::schema_types::Variant;

// ─── File header + struct + primitive fields ─────────────────────────

TEST_CASE("emit_capnp: file id + struct with primitives", "[emit_capnp]")
{
   Schema s;
   s.package = Package{.name    = "test:pkg",
                       .version = "1.0.0",
                       .attributes = {{"capnp_id",
                                       std::string{"@0xdeadbeef00000001"}}}};
   s.insert("point",
            AnyType{Object{.members = {
                              Member{.name = "x",
                                     .type = Box<AnyType>{Int{32, true}}},
                              Member{.name = "y",
                                     .type = Box<AnyType>{Int{32, true}}}}}});

   auto text = psio::emit_capnp(s);

   CHECK(contains(text, "@0xdeadbeef00000001;"));
   CHECK(contains(text, "struct Point {"));
   CHECK(contains(text, "x @0 :Int32;"));
   CHECK(contains(text, "y @1 :Int32;"));
}

TEST_CASE("emit_capnp: snake_case identifiers convert to Pascal/camel",
          "[emit_capnp]")
{
   Schema s;
   s.insert("wall_clock_state",
            AnyType{Object{.members = {
                              Member{.name = "high_res_time",
                                     .type = Box<AnyType>{Int{64, false}}}}}});

   auto text = psio::emit_capnp(s);

   CHECK(contains(text, "struct WallClockState {"));
   CHECK(contains(text, "highResTime @0 :UInt64;"));
}

// ─── Variant → struct with anonymous union ───────────────────────────

TEST_CASE("emit_capnp: Variant emits struct with anonymous union",
          "[emit_capnp]")
{
   Schema s;
   s.insert("status",
            AnyType{Variant{.members = {
                               Member{.name = "ok",
                                      .type = Box<AnyType>{Tuple{}}},
                               Member{.name = "err",
                                      .type = Box<AnyType>{Int{32, false}}}}}});

   auto text = psio::emit_capnp(s);

   CHECK(contains(text, "struct Status {"));
   CHECK(contains(text, "union {"));
   CHECK(contains(text, "ok @0 :Void;"));
   CHECK(contains(text, "err @1 :UInt32;"));
}

// ─── List, Text, Data ────────────────────────────────────────────────

TEST_CASE("emit_capnp: list / text / data type spellings", "[emit_capnp]")
{
   Schema s;
   s.insert(
      "blob",
      AnyType{Object{
         .members = {
            // string field
            Member{.name = "name",
                   .type = Box<AnyType>{Custom{
                      Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                      "string"}}},
            // raw byte array → Data
            Member{.name = "payload",
                   .type = Box<AnyType>{Custom{
                      Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                      "hex"}}},
            // List of u32
            Member{.name = "ids",
                   .type = Box<AnyType>{
                      List{Box<AnyType>{Int{32, false}}}}}}}});

   auto text = psio::emit_capnp(s);

   CHECK(contains(text, "name @0 :Text;"));
   CHECK(contains(text, "payload @1 :Data;"));
   CHECK(contains(text, "ids @2 :List(UInt32);"));
}

// ─── Resource → interface ────────────────────────────────────────────

TEST_CASE("emit_capnp: Resource emits as interface with methods",
          "[emit_capnp]")
{
   Schema s;
   Resource r;
   r.name    = "pollable";
   r.methods = {
      Func{.name = "ready",
           .params = {},
           .result = Box<AnyType>{Custom{Box<AnyType>{Int{1, false}}, "bool"}}},
      Func{.name = "block"}};
   s.insert("pollable", AnyType{r});

   auto text = psio::emit_capnp(s);

   CHECK(contains(text, "interface Pollable {"));
   CHECK(contains(text, "ready @0 () -> (result :Bool);"));
   CHECK(contains(text, "block @1 ();"));
}

// ─── User-supplied ordinals via Attribute carrier ────────────────────

TEST_CASE("emit_capnp: explicit ordinal attribute overrides auto",
          "[emit_capnp]")
{
   Schema s;
   s.insert(
      "evolved",
      AnyType{Object{
         .members = {
            Member{.name = "old_field",
                   .type = Box<AnyType>{Int{32, false}},
                   .attributes = {{"ordinal", std::string{"3"}}}},
            Member{.name = "new_field",
                   .type = Box<AnyType>{Int{32, false}},
                   .attributes = {{"ordinal", std::string{"7"}}}}}}});

   auto text = psio::emit_capnp(s);

   CHECK(contains(text, "oldField @3 :UInt32;"));
   CHECK(contains(text, "newField @7 :UInt32;"));
}

// ─── SchemaBuilder → emit_capnp pipeline ─────────────────────────────

namespace test_capnp_emit
{
   struct datetime
   {
      std::uint64_t seconds = 0;
   };
   PSIO_REFLECT(datetime, seconds)
}  // namespace test_capnp_emit
using test_capnp_emit::datetime;

struct capnp_wall_clock
{
   static datetime now();
};

PSIO_PACKAGE(capnp_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(capnp_clocks)

PSIO_INTERFACE(capnp_wall_clock, types(datetime), funcs(func(now)))

TEST_CASE("emit_capnp: SchemaBuilder pipeline produces well-formed text",
          "[emit_capnp][pipeline]")
{
   auto schema = psio::SchemaBuilder{}
                    .insert_interface<capnp_wall_clock>()
                    .build();
   auto text = psio::emit_capnp(schema);

   CHECK(contains(text, "struct Datetime {"));
   CHECK(contains(text, "seconds"));
   CHECK(contains(text, "interface CapnpWallClock {"));
   CHECK(contains(text, "now @0 ()"));
}
