// Tests for psio/protobuf_parser.hpp — `.proto3` text → Schema IR.

#include <psio/emit_protobuf.hpp>
#include <psio/protobuf_parser.hpp>

#include <catch.hpp>

#include <string>
#include <string_view>
#include <variant>

using psio::AnyType;
using psio::Member;
using psio::parse_protobuf;
using psio::Schema;
using psio::schema_types::Attribute;
using psio::schema_types::Custom;
using psio::schema_types::Float;
using psio::schema_types::Int;
using psio::schema_types::List;
using psio::schema_types::Object;
using psio::schema_types::Option;
using psio::schema_types::Tuple;
using psio::schema_types::Type;
using psio::schema_types::Variant;

namespace
{
   const Member* find_member(const std::vector<Member>& members,
                             std::string_view           name)
   {
      for (const auto& m : members)
         if (m.name == name)
            return &m;
      return nullptr;
   }

   bool has_attr(const std::vector<Attribute>& attrs,
                 std::string_view              name)
   {
      for (const auto& a : attrs)
         if (a.name == name)
            return true;
      return false;
   }

   std::string attr_val(const std::vector<Attribute>& attrs,
                        std::string_view              name)
   {
      for (const auto& a : attrs)
         if (a.name == name && a.value)
            return *a.value;
      return {};
   }
}

TEST_CASE("parse_protobuf: package + simple message",
          "[protobuf_parser]")
{
   auto src = R"(
      syntax = "proto3";
      package wasi.io;

      message Point {
         int32 x = 1;
         int32 y = 2;
      }
   )";
   Schema s = parse_protobuf(src);
   CHECK(s.package.name == "wasi:io");
   const auto* t = s.get("point");
   REQUIRE(t);
   const auto* obj = std::get_if<Object>(&t->value);
   REQUIRE(obj);
   REQUIRE(obj->members.size() == 2);
   CHECK(obj->members[0].name == "x");
   CHECK(attr_val(obj->members[0].attributes, "field") == "1");
   CHECK(obj->members[1].name == "y");
   CHECK(attr_val(obj->members[1].attributes, "field") == "2");
}

TEST_CASE("parse_protobuf: scalar type mapping",
          "[protobuf_parser]")
{
   auto src = R"(
      message AllScalars {
         int32 a = 1;
         int64 b = 2;
         uint32 c = 3;
         uint64 d = 4;
         sint32 e = 5;
         sint64 f = 6;
         fixed32 g = 7;
         fixed64 h = 8;
         sfixed32 i = 9;
         sfixed64 j = 10;
         float k = 11;
         double l = 12;
         bool m = 13;
         string n = 14;
         bytes o = 15;
      }
   )";
   Schema s = parse_protobuf(src);
   const auto* t = s.get("all_scalars");
   REQUIRE(t);
   const auto& m = std::get<Object>(t->value).members;

   //  Int round-trips with width + signedness from the proto keyword.
   {
      const auto* a = find_member(m, "a");
      auto&       v = a->type->value;
      REQUIRE(std::holds_alternative<Int>(v));
      CHECK(std::get<Int>(v).bits == 32);
      CHECK(std::get<Int>(v).isSigned == true);
      CHECK(!has_attr(a->attributes, "pb_fixed"));
      CHECK(!has_attr(a->attributes, "pb_sint"));
   }
   //  sint*  → pb_sint hint, signed varint.
   {
      const auto* e = find_member(m, "e");
      REQUIRE(std::holds_alternative<Int>(e->type->value));
      CHECK(has_attr(e->attributes, "pb_sint"));
   }
   //  fixed* → pb_fixed hint, unsigned int width preserved.
   {
      const auto* g = find_member(m, "g");
      const auto& v = g->type->value;
      REQUIRE(std::holds_alternative<Int>(v));
      CHECK(std::get<Int>(v).isSigned == false);
      CHECK(std::get<Int>(v).bits == 32);
      CHECK(has_attr(g->attributes, "pb_fixed"));
   }
   //  sfixed* → pb_fixed hint, signed.
   {
      const auto* i = find_member(m, "i");
      const auto& v = i->type->value;
      REQUIRE(std::holds_alternative<Int>(v));
      CHECK(std::get<Int>(v).isSigned == true);
      CHECK(has_attr(i->attributes, "pb_fixed"));
   }
   //  float / double → Float{8,23} / Float{11,52}.
   {
      const auto* k = find_member(m, "k");
      REQUIRE(std::holds_alternative<Float>(k->type->value));
      CHECK(std::get<Float>(k->type->value).exp == 8);
      const auto* l = find_member(m, "l");
      CHECK(std::get<Float>(l->type->value).exp == 11);
   }
   //  bool / string round-trip via Custom carrier.
   {
      const auto* mb = find_member(m, "m");
      REQUIRE(std::holds_alternative<Custom>(mb->type->value));
      CHECK(std::get<Custom>(mb->type->value).id == "bool");
      const auto* n = find_member(m, "n");
      REQUIRE(std::holds_alternative<Custom>(n->type->value));
      CHECK(std::get<Custom>(n->type->value).id == "string");
   }
   //  bytes → bare List<u8>.
   {
      const auto* o = find_member(m, "o");
      REQUIRE(std::holds_alternative<List>(o->type->value));
   }
}

TEST_CASE("parse_protobuf: repeated and optional",
          "[protobuf_parser]")
{
   auto src = R"(
      message Msg {
         repeated int32 ids = 1;
         optional int32 maybe = 2;
      }
   )";
   Schema s = parse_protobuf(src);
   const auto& m = std::get<Object>(s.get("msg")->value).members;

   const auto* ids = find_member(m, "ids");
   REQUIRE(std::holds_alternative<List>(ids->type->value));
   const auto& list = std::get<List>(ids->type->value);
   REQUIRE(std::holds_alternative<Int>(list.type->value));
   CHECK(std::get<Int>(list.type->value).bits == 32);

   const auto* maybe = find_member(m, "maybe");
   REQUIRE(std::holds_alternative<Option>(maybe->type->value));
}

TEST_CASE("parse_protobuf: enum → Variant of empty cases",
          "[protobuf_parser]")
{
   auto src = R"(
      enum Color {
         RED = 0;
         GREEN = 1;
         BLUE = 2;
      }
   )";
   Schema s = parse_protobuf(src);
   const auto* t = s.get("color");
   REQUIRE(t);
   REQUIRE(std::holds_alternative<Variant>(t->value));
   const auto& cases = std::get<Variant>(t->value).members;
   REQUIRE(cases.size() == 3);
   CHECK(cases[0].name == "RED");
   CHECK(cases[2].name == "BLUE");
   //  Each case is a unit Tuple.
   CHECK(std::holds_alternative<Tuple>(cases[0].type->value));
}

TEST_CASE("parse_protobuf: oneof body collapses to Variant",
          "[protobuf_parser]")
{
   auto src = R"(
      message Result {
         oneof value {
            int32 ok = 1;
            string err = 2;
         }
      }
   )";
   Schema s = parse_protobuf(src);
   const auto* t = s.get("result");
   REQUIRE(t);
   REQUIRE(std::holds_alternative<Variant>(t->value));
   const auto& cases = std::get<Variant>(t->value).members;
   REQUIRE(cases.size() == 2);
   CHECK(cases[0].name == "ok");
   CHECK(cases[1].name == "err");
}

TEST_CASE("parse_protobuf: skips imports + options",
          "[protobuf_parser]")
{
   auto src = R"(
      syntax = "proto3";
      import "google/protobuf/any.proto";
      option java_package = "com.example.foo";

      message Empty {}
   )";
   Schema s = parse_protobuf(src);
   CHECK(s.get("empty"));
}

TEST_CASE("parse_protobuf: round-trip emit→parse→emit",
          "[protobuf_parser]")
{
   //  Build a Schema by parsing a hand-written proto, emit it, parse
   //  the emit, compare.  Field numbers and pb_fixed/pb_sint hints
   //  must survive the trip.
   auto src = R"(
      syntax = "proto3";
      package demo;

      message Bag {
         int32 a = 1;
         sint32 b = 2;
         sfixed64 c = 3;
         repeated int32 ids = 7;
         optional string name = 8;
      }
   )";
   Schema first  = parse_protobuf(src);
   auto   text   = psio::emit_protobuf(first);
   Schema second = parse_protobuf(text);

   //  Spot-check: fields survive, hints survive, field numbers survive.
   const auto& m = std::get<Object>(second.get("bag")->value).members;
   const auto* b = find_member(m, "b");
   CHECK(has_attr(b->attributes, "pb_sint"));
   CHECK(attr_val(b->attributes, "field") == "2");
   const auto* c = find_member(m, "c");
   CHECK(has_attr(c->attributes, "pb_fixed"));
   CHECK(attr_val(c->attributes, "field") == "3");
   const auto* ids = find_member(m, "ids");
   CHECK(std::holds_alternative<List>(ids->type->value));
   CHECK(attr_val(ids->attributes, "field") == "7");
   const auto* name = find_member(m, "name");
   CHECK(std::holds_alternative<Option>(name->type->value));
}

TEST_CASE("parse_protobuf: explicit field numbers preserved",
          "[protobuf_parser]")
{
   auto src = R"(
      message M {
         int32 first = 7;
         int32 second = 11;
      }
   )";
   Schema s = parse_protobuf(src);
   const auto& m = std::get<Object>(s.get("m")->value).members;
   CHECK(attr_val(find_member(m, "first")->attributes, "field") == "7");
   CHECK(attr_val(find_member(m, "second")->attributes, "field") == "11");
}

TEST_CASE("parse_protobuf: stray semicolons and comments tolerated",
          "[protobuf_parser]")
{
   auto src = R"(
      // header comment
      syntax = "proto3";

      message M {
         /* block
            comment */
         int32 a = 1;;
         int32 b = 2;
      }
   )";
   Schema s = parse_protobuf(src);
   const auto& m = std::get<Object>(s.get("m")->value).members;
   CHECK(m.size() == 2);
}
