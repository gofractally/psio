// psio::parse_protobuf: `.proto` text → Schema IR. That the IR keeps the
// file's descriptor is protobuf_schema_tests.cpp (which needs protoc); here:
// what the IR holds, what is refused and where, and that the parser is
// linear in the text.

#include <psio/emit_protobuf.hpp>
#include <psio/protobuf_parser.hpp>

#include <catch.hpp>

#include <chrono>
#include <string>

using namespace psio::schema_types;

namespace
{
   const std::string* attribute(const std::vector<Attribute>& attrs, std::string_view name)
   {
      for (const auto& a : attrs)
         if (a.name == name)
            return a.value ? &*a.value : nullptr;
      return nullptr;
   }
   bool has(const std::vector<Attribute>& attrs, std::string_view name)
   {
      for (const auto& a : attrs)
         if (a.name == name)
            return true;
      return false;
   }
   const Object& message(const Schema& s, std::string_view name)
   {
      const AnyType* t = s.get(name);
      REQUIRE(t);
      return std::get<Object>(t->value);
   }
   const Member& member(const Object& o, std::string_view name)
   {
      for (const auto& m : o.members)
         if (m.name == name)
            return m;
      FAIL("no member " << name);
      return o.members.front();
   }
   std::string refusal(std::string_view text)
   {
      try
      {
         (void)psio::parse_protobuf(text);
      }
      catch (const psio::protobuf_parse_error& e)
      {
         return e.what();
      }
      return {};
   }

   const char* const sample = R"(// a comment
syntax = "proto3";
package demo.v1;
import public "other.proto";
option java_package = "a;b";

enum Color { option allow_alias = true; NONE = 0; Red = 5; ROUGE = 5 [deprecated = true]; Neg = -7; }

message Outer {
  reserved 9, 20 to 30;
  option deprecated = true;
  message Mid { message Leaf { sint32 CamelCase = 1; } Leaf leaf = 1; }
  int32 a = 1;
  optional string name = 2 [json_name = "n", deprecated = true];
  repeated Mid.Leaf leaves = 3 [packed = false];
  map<sfixed64, Mid> mids = 4;
  oneof first { fixed32 x = 5; Mid.Leaf y = 6; }
  bytes raw = 7;
  oneof second { Color tint = 8; }
  other.Thing imported = 10;
  .demo.v1.Outer.Mid absolute = 11;
}

service Api { rpc Get (Outer) returns (stream Outer.Mid) { option deprecated = true; } }
)";
}  // namespace

TEST_CASE("parse_protobuf: what the IR keeps", "[protobuf_parser]")
{
   const Schema s = psio::parse_protobuf(sample);
   CHECK(s.package.name == "demo:v1");
   CHECK(!has(s.package.attributes, "proto_syntax"));
   CHECK(*attribute(s.package.attributes, "proto_import") == "public \"other.proto\"");
   CHECK(*attribute(s.package.attributes, "proto_option") == "java_package = \"a;b\"");

   // An enum's values, as written.
   const Enum& color = std::get<Enum>(s.get("Color")->value);
   REQUIRE(color.members.size() == 4);
   CHECK(color.members[1].name == "Red");
   CHECK(color.members[1].value == 5);
   CHECK(color.members[2].value == 5);
   CHECK(static_cast<std::int64_t>(color.members[3].value) == -7);
   CHECK(std::get<Int>(color.type->value) == Int{32, true});
   for (const auto& entry : s.types)
      if (entry.name == "Color")
      {
         CHECK(*attribute(entry.attributes, "proto_option") == "allow_alias = true");
         CHECK(*attribute(entry.attributes, "proto_option.ROUGE") == "deprecated = true");
      }

   // Nesting to any depth, names as written.
   CHECK(s.get("Outer.Mid"));
   const Object& leaf = message(s, "Outer.Mid.Leaf");
   CHECK(leaf.members[0].name == "CamelCase");
   CHECK(*attribute(leaf.members[0].attributes, "protobuf") == "sint");

   const Object& outer = message(s, "Outer");
   CHECK(*attribute(outer.attributes, "proto_reserved") == "9, 20 to 30");
   CHECK(*attribute(outer.attributes, "proto_option") == "deprecated = true");
   REQUIRE(outer.members.size() == 9);

   const Member& name = member(outer, "name");
   CHECK(std::holds_alternative<Option>(name.type->value));
   CHECK(*attribute(name.attributes, "field") == "2");
   CHECK(has(name.attributes, "deprecated"));
   CHECK(*attribute(name.attributes, "proto_option") == "json_name = \"n\"");

   // A reference is the declaration's name.
   const Member& leaves = member(outer, "leaves");
   CHECK(std::get<Type>(std::get<List>(leaves.type->value).type->value).type == "Outer.Mid.Leaf");
   CHECK(*attribute(leaves.attributes, "proto_option") == "packed = false");
   CHECK(std::get<Type>(member(outer, "absolute").type->value).type == "Outer.Mid");
   // … or, for a type of an imported file, as written.
   CHECK(std::get<Type>(member(outer, "imported").type->value).type == "other.Thing");

   const Member& mids = member(outer, "mids");
   const Custom& map  = std::get<Custom>(mids.type->value);
   CHECK(map.id == "map");
   const Tuple& entry = std::get<Tuple>(std::get<List>(map.type->value).type->value);
   CHECK(std::get<Int>(entry.members[0]->value) == Int{64, true});
   CHECK(std::get<Type>(entry.members[1]->value).type == "Outer.Mid");
   CHECK(*attribute(mids.attributes, "protobuf_key") == "fixed");

   // A oneof beside fields; two of them stay two.
   const Variant& first = std::get<Variant>(member(outer, "first").type->value);
   REQUIRE(first.members.size() == 3);
   CHECK(std::holds_alternative<Tuple>(first.members[0].type->value));  // none set
   CHECK(first.members[1].name == "x");
   CHECK(*attribute(first.members[1].attributes, "field") == "5");
   CHECK(*attribute(first.members[1].attributes, "protobuf") == "fixed");
   CHECK(*attribute(first.members[2].attributes, "field") == "6");
   const Variant& second = std::get<Variant>(member(outer, "second").type->value);
   CHECK(second.members.size() == 2);
   CHECK(*attribute(member(outer, "second").attributes, "field") == "8");

   REQUIRE(s.interfaces.size() == 1);
   const Func& get = s.interfaces[0].funcs[0];
   CHECK(get.name == "Get");
   CHECK(std::get<Type>(get.params[0].type->value).type == "Outer");
   CHECK(std::get<Type>((*get.result)->value).type == "Outer.Mid");
   CHECK(has(get.attributes, "proto_server_streaming"));
   CHECK(!has(get.attributes, "proto_client_streaming"));
   CHECK(*attribute(get.attributes, "proto_option") == "deprecated = true");

   // Text → IR → text → IR is stable.
   const std::string once = psio::emit_protobuf(s);
   CHECK(psio::emit_protobuf(psio::parse_protobuf(once)) == once);
   CHECK(psio::parse_protobuf(once) == psio::parse_protobuf(psio::emit_protobuf(psio::parse_protobuf(once))));
}

TEST_CASE("parse_protobuf: proto2", "[protobuf_parser]")
{
   const Schema s = psio::parse_protobuf(
      "message M { required int32 a = 1; optional string b = 2 [default = \"x;y\"];\n"
      "  repeated M ms = 3; optional E e = 4 [default = B]; enum E { A = 1; B = 2; }\n"
      "  extensions 100 to max; }");
   CHECK(*attribute(s.package.attributes, "proto_syntax") == "proto2");
   const Object& m = message(s, "M");
   CHECK(has(member(m, "a").attributes, "required"));
   CHECK(*attribute(member(m, "b").attributes, "default") == "\"x;y\"");
   CHECK(*attribute(member(m, "e").attributes, "default") == "B");
   CHECK(*attribute(m.attributes, "proto_extensions") == "100 to max");
   CHECK(std::get<Enum>(s.get("M.E")->value).members[0].value == 1);
   const std::string text = psio::emit_protobuf(s);
   CHECK(text.find("required int32 a = 1;") != std::string::npos);
   CHECK(text.find("optional string b = 2 [default = \"x;y\"];") != std::string::npos);
   CHECK(psio::emit_protobuf(psio::parse_protobuf(text)) == text);
}

TEST_CASE("parse_protobuf: scoping", "[protobuf_parser]")
{
   // The innermost declaration of the first component wins.
   const Schema s = psio::parse_protobuf(
      "syntax = \"proto3\"; package p.q;\n"
      "message X { int32 a = 1; }\n"
      "message A { message X { int32 b = 1; } message B { X inner = 1; .p.q.X outer = 2; p.q.X also = 3; } }");
   const Object& b = message(s, "A.B");
   CHECK(std::get<Type>(b.members[0].type->value).type == "A.X");
   CHECK(std::get<Type>(b.members[1].type->value).type == "X");
   CHECK(std::get<Type>(b.members[2].type->value).type == "X");
   const std::string text = psio::emit_protobuf(s);
   CHECK(text.find(".p.q.A.X inner = 1;") != std::string::npos);
   CHECK(text.find(".p.q.X outer = 2;") != std::string::npos);
}

TEST_CASE("parse_protobuf: what is refused, and where", "[protobuf_parser]")
{
   CHECK(refusal("edition = \"2023\";") ==
         "proto:1:1: editions are not read (proto2 and proto3 are)");
   CHECK(refusal("syntax = \"proto3\";\nextend Foo { int32 a = 1; }") ==
         "proto:2:1: extensions are not read");
   CHECK(refusal("message M { optional group G = 1 { } }") == "proto:1:22: groups are not read");
   CHECK(refusal("syntax = \"proto3\"; message M { int32 a = 1; int32 b = 1; }") ==
         "proto:1:55: the field number 1 is used twice");
   CHECK(refusal("syntax = \"proto3\"; message M { int32 a = 0; }") ==
         "proto:1:42: a field number is within 1..536870911 and outside 19000..19999");
   CHECK(refusal("syntax = \"proto3\"; message M { int32 a = 19000; }") ==
         "proto:1:42: a field number is within 1..536870911 and outside 19000..19999");
   CHECK(refusal("syntax = \"proto3\"; message M { int32 a = 99999999999999999999; }") ==
         "proto:1:42: expected a field number");
   CHECK(refusal("syntax = \"proto3\"; message M { Missing a = 1; }") ==
         "proto:1:32: 'Missing' is not declared");
   CHECK(refusal("syntax = \"proto3\"; message M { required int32 a = 1; }") ==
         "proto:1:32: proto3 has no required fields");
   CHECK(refusal("syntax = \"proto3\"; enum E { A = 1; }") ==
         "proto:1:25: proto3: the first value of an enum is zero");
   CHECK(refusal("syntax = \"proto3\"; message M {} message M {}") ==
         "proto:1:41: 'M' is declared twice");
   CHECK(refusal("syntax = \"proto3\"; message M { map<float, int32> m = 1; }") ==
         "proto:1:36: a map's key is an integer, a bool or a string");
   CHECK(refusal("message M { int32 a = 1; }") ==
         "proto:1:13: proto2: a field is optional, required or repeated");
   CHECK(refusal("syntax = \"proto4\";") == "proto:1:10: expected \"proto2\" or \"proto3\"");
   // Text that does not end.
   CHECK(refusal("enum E {") == "proto:1:9: expected an enum value name");
   CHECK(refusal("enum E { 5") == "proto:1:10: expected an enum value name");
   CHECK(refusal("message M { /* never") == "proto:1:21: a comment that does not end");
   CHECK(refusal("option a = \"x") == "proto:1:14: a string that does not end");
   CHECK(refusal("message M {") == "proto:1:12: a message that does not end");
   CHECK(refusal("option x = { a: [1, 2 ;") == "proto:1:24: unexpected end of the file");
   CHECK(refusal("syntax = \"proto3\"; message M { oneof o { int32 a = 1;") ==
         "proto:1:54: a oneof that does not end");
   std::string deep;
   for (int i = 0; i < 100; ++i)
      deep += "message M" + std::to_string(i) + " { ";
   CHECK(refusal(deep).find("messages nested too deeply") != std::string::npos);
}

TEST_CASE("parse_protobuf: linear in the text", "[protobuf_parser]")
{
   const auto seconds = [](unsigned count) {
      std::string text = "syntax = \"proto3\";\n";
      for (unsigned i = 0; i < count; ++i)
         text += "message M" + std::to_string(i) + " { int32 a = 1; M" +
                 std::to_string(i ? i - 1 : 0) + " prev = 2; }\n";
      const auto   begin = std::chrono::steady_clock::now();
      const Schema s     = psio::parse_protobuf(text);
      const auto   end   = std::chrono::steady_clock::now();
      CHECK(s.types.size() == count);
      return std::chrono::duration<double>(end - begin).count();
   };
   (void)seconds(1000);
   const double small = seconds(40000);
   const double large = seconds(320000);
   INFO("40,000 messages: " << small << " s; 320,000: " << large << " s");
   // Eight times the text: eight times the time, not sixty-four.
   CHECK(large < small * 24);
   CHECK(small < 1.0);
}
