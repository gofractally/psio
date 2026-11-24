// psio::emit_protobuf: the `.proto` text of a Schema IR. That protoc
// accepts it and compiles it to the codec's wire is
// protobuf_schema_tests.cpp and protobuf_interop_tests.cpp (which need
// protoc); here: the text itself, and what is refused.

#include <psio/emit_protobuf.hpp>
#include <psio/protobuf_parser.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <array>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "protobuf_shapes.hpp"

namespace emit_pb_test
{
   enum class Kind : std::uint16_t { plain, fancy = 5 };
   PSIO_REFLECT_ENUM(Kind, plain, fancy)
   enum class Mode : std::int8_t { slow = 1, plain = 2 };  // no zero; `plain` again
   PSIO_REFLECT_ENUM(Mode, slow, plain)

   struct Inner
   {
      std::int64_t z = 0;
   };
   PSIO_REFLECT(Inner, attr(z, pb_sint))

   struct Record
   {
      std::int32_t                               id = 0;
      std::string                                name;
      Kind                                       kind{};
      Mode                                       mode{};
      std::optional<double>                      ratio;
      std::vector<std::uint32_t>                 ids;
      std::vector<Inner>                         inners;
      std::map<std::string, Inner>               by_name;
      std::variant<std::monostate, int, Inner>   pick;
      std::variant<int, std::string>             other;
      std::vector<char>                          raw;
      std::int32_t                               old = 0;
      std::uint64_t                              big = 9;
   };
   PSIO_REFLECT(Record,
                id,
                name,
                kind,
                mode,
                ratio,
                attr(ids, pb_fixed),
                inners,
                by_name,
                attr(pick, field<20>),
                attr(other, field<30>),
                attr(raw, field<40>),
                attr(old, field<41> | deprecated_spec{.version = "3"}),
                attr(big, field<42>))

   struct ListOfLists
   {
      std::vector<std::vector<std::int32_t>> v;
   };
   PSIO_REFLECT(ListOfLists, v)
   struct OptionalList
   {
      std::optional<std::vector<std::int32_t>> v;
   };
   PSIO_REFLECT(OptionalList, v)
   struct WithTuple
   {
      std::tuple<int, int> v;
   };
   PSIO_REFLECT(WithTuple, v)
   struct WithArray
   {
      std::array<std::int32_t, 3> v{};
   };
   PSIO_REFLECT(WithArray, v)
   struct Clash
   {
      std::variant<int, std::string> v;
      std::int32_t                   after = 0;
   };
   PSIO_REFLECT(Clash, v, after)

   template <typename T>
   std::string refusal()
   {
      psio::SchemaBuilder b;
      b.insert<T>("T");
      try
      {
         (void)psio::emit_protobuf(b.build());
      }
      catch (const std::runtime_error& e)
      {
         return e.what();
      }
      return {};
   }
}  // namespace emit_pb_test

TEST_CASE("emit_protobuf: a reflected record, as the codec writes it", "[emit_protobuf]")
{
   psio::SchemaBuilder b;
   b.insert<emit_pb_test::Record>("Record");
   auto s         = b.build();
   s.package.name = "app:data";
   CHECK(psio::emit_protobuf(s) == R"(syntax = "proto3";

package app.data;

message Record {
  int32 id = 1;
  string name = 2;
  Record_kind kind = 3;
  Record_mode mode = 4;
  optional double ratio = 5;
  repeated fixed32 ids = 6;
  repeated Inner inners = 7;
  map<string, Inner> by_name = 8;
  oneof pick {
    int32 int32 = 20;
    Inner Inner = 21;
  }
  oneof other {
    int32 other_int32 = 30;
    string string = 31;
  }
  bytes raw = 40;
  int32 old = 41 [deprecated = true];
  uint64 big = 42;
}

message Inner {
  sint64 z = 1;
}

enum Record_kind {
  plain = 0;
  fancy = 5;
}

enum Record_mode {
  Record_mode_UNSPECIFIED = 0;
  slow = 1;
  Record_mode_plain = 2;
}

)");
}

TEST_CASE("emit_protobuf: the shapes of the codec, and back through the parser", "[emit_protobuf]")
{
   const std::string text = psio::emit_protobuf(protobuf_shapes_schema());
   CHECK(text.find("map<string, PbRepeated> lists = 9;") != std::string::npos);
   CHECK(text.find("repeated sfixed32 sf = 11;") != std::string::npos);
   CHECK(text.find("PbScalars_level_UNSPECIFIED = 0;") != std::string::npos);
   // The text is a fixed point of parse → emit.
   CHECK(psio::emit_protobuf(psio::parse_protobuf(text)) == text);
}

TEST_CASE("emit_protobuf: what protobuf does not have is an error", "[emit_protobuf]")
{
   using namespace emit_pb_test;
   CHECK(refusal<ListOfLists>() ==
         "emit_protobuf: member 'v' of 'ListOfLists': a list of lists has no protobuf type");
   CHECK(refusal<OptionalList>() ==
         "emit_protobuf: member 'v' of 'OptionalList': a list of lists has no protobuf type");
   CHECK(refusal<WithTuple>() == "emit_protobuf: member 'v' of 'WithTuple': a tuple has no protobuf type");
   CHECK(refusal<WithArray>() ==
         "emit_protobuf: member 'v' of 'WithArray': a fixed array that is not bytes has no protobuf type");
   CHECK(refusal<Clash>() ==
         "emit_protobuf: member 'after' of 'Clash': the field number 2 is used twice (a variant "
         "takes one number per alternative)");

   using namespace psio::schema_types;
   const auto with = [](AnyType type, std::vector<Attribute> attrs) {
      Schema s;
      Object o;
      o.members.push_back(Member{"m", Box<AnyType>{std::move(type)}, std::move(attrs)});
      s.types.push_back(Member{"T", Box<AnyType>{AnyType{std::move(o)}}, {}});
      try
      {
         (void)emit_protobuf(s);
      }
      catch (const std::runtime_error& e)
      {
         return std::string{e.what()};
      }
      return std::string{};
   };
   CHECK(with(Int{128, false}, {}) ==
         "emit_protobuf: member 'm' of 'T': an integer of 128 bits has no protobuf type");
   CHECK(with(Int{32, false}, {Attribute{"protobuf", "sint"}}) ==
         "emit_protobuf: member 'm' of 'T': sint is for signed integers");
   CHECK(with(Float{11, 53}, {Attribute{"protobuf", "fixed"}}) ==
         "emit_protobuf: member 'm' of 'T': the 'protobuf' attribute is for integers");
   CHECK(with(Int{32, true}, {Attribute{"field", "0"}}) ==
         "emit_protobuf: member 'm' of 'T': the field number 0 is not one protobuf allows");
   CHECK(with(Int{32, true}, {Attribute{"field", "19500"}}) ==
         "emit_protobuf: member 'm' of 'T': the field number 19500 is not one protobuf allows");
   CHECK(with(Int{32, true}, {Attribute{"field", "99999999999999999999999"}}) ==
         "emit_protobuf: member 'm' of 'T': the 'field' attribute is not a number");
   CHECK(with(Int{32, true}, {Attribute{"field", "536870911"}}).empty());
   CHECK(with(Type{"Nowhere"}, {}) == "fbs_layout: unknown type 'Nowhere'");
}

TEST_CASE("emit_protobuf: names that are not identifiers", "[emit_protobuf]")
{
   using namespace psio::schema_types;
   Schema s;
   Object o;
   o.members.push_back(Member{"a-b", Box<AnyType>{AnyType{Type{"@7"}}}, {}});
   o.members.push_back(Member{"a_b", Box<AnyType>{AnyType{Int{32, true}}}, {}});
   s.types.push_back(Member{"app::Config", Box<AnyType>{AnyType{std::move(o)}}, {}});
   s.types.push_back(Member{"@7", Box<AnyType>{AnyType{Object{}}}, {}});
   CHECK(psio::emit_protobuf(s) == R"(syntax = "proto3";

message app__Config {
  app__Config_a_b a_b = 1;
  int32 a_b_2 = 2;
}

message app__Config_a_b {
}

)");
}
