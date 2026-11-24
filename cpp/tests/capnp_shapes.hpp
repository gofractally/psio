#pragma once
// The record types the Cap'n Proto schema tests share, and their Schema IR
// as the schema builder makes it from their reflection: what
// capnp_schema_emit.cpp writes as `.capnp` text for the official compiler
// (capnp_schema_interop_tests.cpp exchanges messages through the compiled
// schema) and capnp_schema_tests.cpp compares layouts of.
//
// At global scope: an annotation specializes a psio template, which C++
// only allows from an enclosing namespace.

#include <psio/annotate.hpp>
#include <psio/capnp.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "capnp_layout_fixture.hpp"

enum class CpGrade : std::uint8_t
{
   low,
   mid,
   high
};

struct CpProfile
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool   operator==(const CpProfile&, const CpProfile&) = default;
};
PSIO_REFLECT(CpProfile, id, name, email, age, verified)

struct CpItem
{
   std::string   product;
   std::uint32_t qty        = 1;
   double        unit_price = 0;
   friend bool   operator==(const CpItem&, const CpItem&) = default;
};
PSIO_REFLECT(CpItem, product, qty, unit_price)

struct CpOrder
{
   std::uint64_t       id = 0;
   CpProfile           customer;
   std::vector<CpItem> items;
   double              total = 0;
   std::string         note;
   friend bool         operator==(const CpOrder&, const CpOrder&) = default;
};
PSIO_REFLECT(CpOrder, id, customer, items, total, note)

struct CpMixed
{
   bool         flag  = false;
   std::int8_t  small = 0;
   std::int16_t level = 7;
   float        ratio = 1.5f;
   bool         on    = true;
   std::variant<std::monostate, std::uint32_t, std::string, CpProfile, double> payment;
   std::optional<std::uint32_t>           score;
   std::optional<std::string>             memo;
   std::vector<bool>                      bits;
   std::vector<std::uint8_t>              blob;
   std::vector<std::string>               names;
   std::vector<std::vector<std::int16_t>> grid;
   std::vector<CpProfile>                 people;
   std::uint64_t                          wide  = 0;
   CpGrade                                grade = CpGrade::high;
   std::array<std::uint16_t, 3>           triple{};
   std::optional<CpItem>                  extra;
   std::vector<CpGrade>                   grades;
   std::optional<bool>                    maybe;
   friend bool operator==(const CpMixed&, const CpMixed&) = default;
};
PSIO_REFLECT(CpMixed, flag, small, level, ratio, on, payment, score, memo, bits, blob, names, grid,
             people, wide, grade, triple, extra, grades, maybe)

// Field numbers that are not the declaration order: ordinals follow them.
struct CpRenumbered
{
   std::int32_t                           c = 0;
   std::string                            a;
   std::variant<std::int8_t, std::string> u;
   std::int64_t                           b = 0;
   friend bool operator==(const CpRenumbered&, const CpRenumbered&) = default;
};
PSIO_REFLECT(CpRenumbered, attr(c, field<3>), attr(a, field<1>), attr(u, field<2>),
             attr(b, field<4>))

struct CpDefaults
{
   std::int8_t   a = -5;
   std::uint64_t b = ~std::uint64_t{0};
   double        d = -2.5;
   float         f = 0.25f;
   bool          t = true;
   CpGrade       g = CpGrade::mid;
   std::uint16_t u = 65535;
   std::int32_t  z = 0;
   friend bool   operator==(const CpDefaults&, const CpDefaults&) = default;
};
PSIO_REFLECT(CpDefaults, a, b, d, f, t, g, u, z)

inline psio::Schema capnp_shapes_schema()
{
   psio::SchemaBuilder b;
   b.insert<CpOrder>("CpOrder");
   b.insert<CpMixed>("CpMixed");
   b.insert<CpRenumbered>("CpRenumbered");
   b.insert<CpDefaults>("CpDefaults");
   auto s         = b.build();
   s.package.name = "psio:test";
   s.package.attributes.push_back({"capnp_id", "0xd1e5a6c4b3f29788"});
   // The generated C++ goes in a namespace of its own (the types above
   // have the names of the structs).
   s.package.attributes.push_back({"capnp_decl", "using Cxx = import \"/capnp/c++.capnp\";"});
   s.package.attributes.push_back({"capnp_decl", "$Cxx.namespace(\"cpe\");"});
   return s;
}

inline CpOrder cp_sample_order()
{
   CpOrder o;
   o.id       = 0x1122334455667788ull;
   o.customer = {77, "Alice Example", "alice@example.org", 37, true};
   for (int i = 0; i < 5; ++i)
      o.items.push_back({"product-" + std::to_string(i), std::uint32_t(i + 1), 1.5 * i});
   o.total = 1234.5;
   o.note  = "leave at the door";
   return o;
}

inline CpMixed cp_sample_mixed()
{
   CpMixed m;
   m.flag    = true;
   m.small   = -7;
   m.level   = -300;
   m.ratio   = 2.75f;
   m.on      = false;
   m.payment = CpProfile{5, "payer", "", 1, false};
   m.score   = 99;
   m.memo    = "memo";
   m.bits    = {true, false, true, true, false, false, true, false, true};
   m.blob    = {0, 1, 254, 255};
   m.names   = {"a", "", "longer name"};
   m.grid    = {{1, -2, 3}, {}, {-32768, 32767}};
   m.people  = {{1, "one", "1@x", 11, true}, {2, "two", "", 22, false}};
   m.wide    = 0xfedcba9876543210ull;
   m.grade   = CpGrade::low;
   m.triple  = {1, 2, 65535};
   m.extra   = CpItem{"extra", 3, 0.5};
   m.grades  = {CpGrade::high, CpGrade::low};
   m.maybe   = false;
   return m;
}
