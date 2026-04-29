// Tests for psio/emit_pssz.hpp — Schema IR → pSSZ bytes (and back).

#include <psio/emit_pssz.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>

using psio::AnyType;
using psio::Func;
using psio::Interface;
using psio::Member;
using psio::Package;
using psio::Schema;
using psio::schema_types::Box;
using psio::schema_types::Int;
using psio::schema_types::Object;
using psio::schema_types::Type;

TEST_CASE("emit_pssz: round-trips a populated Schema", "[emit_pssz]")
{
   Schema s;
   s.package = Package{.name = "wasi:io", .version = "0.2.3"};
   s.insert("u32", AnyType{Int{32, false}});
   s.insert("instant",
            AnyType{Object{.members = {Member{
                              .name = "nanos",
                              .type = Box<AnyType>{Type{"u32"}}}}}});
   s.interfaces.push_back(Interface{
      .name       = "monotonic_clock",
      .type_names = {"instant"},
      .funcs      = {Func{.name = "now",
                          .result = Box<AnyType>{Type{"instant"}}},
                     Func{.name = "resolution",
                          .result = Box<AnyType>{Type{"u32"}}}}});

   auto bytes = psio::emit_pssz(s);
   REQUIRE_FALSE(bytes.empty());

   Schema parsed = psio::parse_pssz(std::span<const char>{bytes});
   CHECK(parsed == s);

   // Idempotence: re-emitting yields identical bytes.
   auto bytes2 = psio::emit_pssz(parsed);
   CHECK(bytes2 == bytes);
}

// ─── Pipeline: SchemaBuilder → emit_pssz → parse_pssz ────────────────

namespace test_emit_pssz
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
}  // namespace test_emit_pssz
using test_emit_pssz::datetime;
using test_emit_pssz::Point;

struct emit_pssz_clock
{
   static datetime now();
};

PSIO_PACKAGE(emit_pssz_pkg, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(emit_pssz_pkg)

PSIO_INTERFACE(emit_pssz_clock, types(datetime), funcs(func(now)))

TEST_CASE("emit_pssz: SchemaBuilder → emit_pssz → parse_pssz pipeline",
          "[emit_pssz][pipeline]")
{
   Schema built = psio::SchemaBuilder{}
                     .insert<Point>("Point")
                     .insert_interface<emit_pssz_clock>()
                     .build();

   auto   bytes  = psio::emit_pssz(built);
   Schema parsed = psio::parse_pssz(std::span<const char>{bytes});
   CHECK(parsed == built);
}
