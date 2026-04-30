// Tests for psio/emit_protobuf.hpp — Schema IR → .proto3 schema text.

#include <psio/emit_protobuf.hpp>
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
using psio::Member;
using psio::Package;
using psio::Schema;
using psio::schema_types::Attribute;
using psio::schema_types::Box;
using psio::schema_types::Custom;
using psio::schema_types::Float;
using psio::schema_types::Int;
using psio::schema_types::List;
using psio::schema_types::Object;
using psio::schema_types::Option;
using psio::schema_types::Tuple;
using psio::schema_types::Type;
using psio::schema_types::Variant;

TEST_CASE("emit_protobuf: header + package", "[emit_protobuf]")
{
   Schema s;
   s.package = Package{.name = "wasi:io", .version = "0.2.3"};
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "syntax = \"proto3\";"));
   CHECK(contains(text, "package wasi.io;"));
}

TEST_CASE("emit_protobuf: message with primitive fields",
          "[emit_protobuf]")
{
   Schema s;
   s.insert("point",
            AnyType{Object{.members = {
                              Member{.name = "x",
                                     .type = Box<AnyType>{Int{32, true}}},
                              Member{.name = "y",
                                     .type = Box<AnyType>{Int{32, true}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "message Point {"));
   CHECK(contains(text, "int32 x = 1;"));
   CHECK(contains(text, "int32 y = 2;"));
}

TEST_CASE("emit_protobuf: int width + signedness mapping",
          "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "all_ints",
      AnyType{Object{
         .members = {
            Member{.name = "a", .type = Box<AnyType>{Int{32, true}}},
            Member{.name = "b", .type = Box<AnyType>{Int{32, false}}},
            Member{.name = "c", .type = Box<AnyType>{Int{64, true}}},
            Member{.name = "d", .type = Box<AnyType>{Int{64, false}}},
            //  8/16-bit Ints widen to 32 — proto3 has no narrower form.
            Member{.name = "e", .type = Box<AnyType>{Int{8, true}}},
            Member{.name = "f", .type = Box<AnyType>{Int{16, false}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "int32 a"));
   CHECK(contains(text, "uint32 b"));
   CHECK(contains(text, "int64 c"));
   CHECK(contains(text, "uint64 d"));
   CHECK(contains(text, "int32 e"));
   CHECK(contains(text, "uint32 f"));
}

TEST_CASE("emit_protobuf: pb_fixed and pb_sint hints",
          "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "msg",
      AnyType{Object{
         .members = {
            Member{.name       = "f32",
                   .type       = Box<AnyType>{Int{32, true}},
                   .attributes = {Attribute{.name = "pb_fixed"}}},
            Member{.name       = "uf32",
                   .type       = Box<AnyType>{Int{32, false}},
                   .attributes = {Attribute{.name = "pb_fixed"}}},
            Member{.name       = "f64",
                   .type       = Box<AnyType>{Int{64, true}},
                   .attributes = {Attribute{.name = "pb_fixed"}}},
            Member{.name       = "z32",
                   .type       = Box<AnyType>{Int{32, true}},
                   .attributes = {Attribute{.name = "pb_sint"}}},
            Member{.name       = "z64",
                   .type       = Box<AnyType>{Int{64, true}},
                   .attributes = {Attribute{.name = "pb_sint"}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "sfixed32 f32"));
   CHECK(contains(text, "fixed32 uf32"));
   CHECK(contains(text, "sfixed64 f64"));
   CHECK(contains(text, "sint32 z32"));
   CHECK(contains(text, "sint64 z64"));
}

TEST_CASE("emit_protobuf: explicit field number from `field` attribute",
          "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "msg",
      AnyType{Object{
         .members = {Member{.name       = "old_field",
                            .type       = Box<AnyType>{Int{32, true}},
                            .attributes = {Attribute{.name  = "field",
                                                     .value = "7"}}},
                     Member{.name       = "new_field",
                            .type       = Box<AnyType>{Int{32, true}},
                            .attributes = {Attribute{.name  = "field",
                                                     .value = "11"}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "int32 old_field = 7;"));
   CHECK(contains(text, "int32 new_field = 11;"));
}

TEST_CASE("emit_protobuf: List<u8> collapses to bytes",
          "[emit_protobuf]")
{
   Schema s;
   s.insert("blob",
            AnyType{Object{.members = {
                              Member{.name = "data",
                                     .type = Box<AnyType>{
                                        List{Box<AnyType>{Int{8, false}}}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "bytes data = 1;"));
}

TEST_CASE("emit_protobuf: List<T> emits repeated", "[emit_protobuf]")
{
   Schema s;
   s.insert("ids",
            AnyType{Object{.members = {
                              Member{.name = "values",
                                     .type = Box<AnyType>{
                                        List{Box<AnyType>{Int{32, true}}}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "repeated int32 values = 1;"));
}

TEST_CASE("emit_protobuf: Option emits optional keyword",
          "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "maybe",
      AnyType{Object{.members = {Member{.name = "count",
                                        .type = Box<AnyType>{Option{
                                           Box<AnyType>{Int{32, true}}}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "optional int32 count = 1;"));
}

TEST_CASE("emit_protobuf: Custom string + bool + hex", "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "msg",
      AnyType{Object{
         .members = {
            Member{.name = "n",
                   .type = Box<AnyType>{
                      Custom{Box<AnyType>{Int{8, false}}, "bool"}}},
            Member{.name = "name",
                   .type = Box<AnyType>{Custom{
                      Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                      "string"}}},
            Member{.name = "h",
                   .type = Box<AnyType>{Custom{
                      Box<AnyType>{List{Box<AnyType>{Int{8, false}}}},
                      "hex"}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "bool n = 1;"));
   CHECK(contains(text, "string name = 2;"));
   CHECK(contains(text, "bytes h = 3;"));
}

TEST_CASE("emit_protobuf: Variant of empty cases → enum",
          "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "color",
      AnyType{Variant{
         .members = {Member{.name = "red", .type = Box<AnyType>{Tuple{}}},
                     Member{.name = "green",
                            .type = Box<AnyType>{Tuple{}}},
                     Member{.name = "blue",
                            .type = Box<AnyType>{Tuple{}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "enum Color {"));
   CHECK(contains(text, "red = 0;"));
   CHECK(contains(text, "green = 1;"));
   CHECK(contains(text, "blue = 2;"));
}

TEST_CASE("emit_protobuf: Variant with payloads → oneof message",
          "[emit_protobuf]")
{
   Schema s;
   s.insert(
      "result",
      AnyType{Variant{
         .members = {Member{.name = "ok", .type = Box<AnyType>{Int{32, true}}},
                     Member{.name = "err",
                            .type = Box<AnyType>{Custom{
                               Box<AnyType>{
                                  List{Box<AnyType>{Int{8, false}}}},
                               "string"}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "message Result {"));
   CHECK(contains(text, "oneof value {"));
   CHECK(contains(text, "int32 ok = 1;"));
   CHECK(contains(text, "string err = 2;"));
}

TEST_CASE("emit_protobuf: Float widths", "[emit_protobuf]")
{
   Schema s;
   s.insert("vec",
            AnyType{Object{.members = {
                              Member{.name = "x",
                                     .type = Box<AnyType>{Float{8, 23}}},
                              Member{.name = "y",
                                     .type = Box<AnyType>{Float{11, 52}}}}}});
   auto text = psio::emit_protobuf(s);
   CHECK(contains(text, "float x"));
   CHECK(contains(text, "double y"));
}
