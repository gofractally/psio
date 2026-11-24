// psio::emit_fbs — Schema IR → `.fbs` text. That `flatc` accepts the text
// and lays it out as psio does is fbs_schema_tests.cpp; that code generated
// from it exchanges buffers with psio::flatbuf is
// flatbuf_schema_interop_tests.cpp. Here: what the text says.

#include <psio/emit_fbs.hpp>
#include <psio/fbs_parser.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace psio::schema_types;

namespace emit_fbs_test
{
   enum class Kind : std::uint16_t
   {
      plain,
      fancy = 5
   };
   PSIO_REFLECT_ENUM(Kind, plain, fancy)

   struct Corner
   {
      std::int16_t x = 0, y = 0;
   };
   PSIO_REFLECT(Corner, definitionWillNotChange(), x, y)

   struct Leaf
   {
      std::string name;
   };
   PSIO_REFLECT(Leaf, name)

   struct Twig
   {
      std::uint32_t length = 0;
   };
   PSIO_REFLECT(Twig, length)

   struct Record
   {
      std::uint64_t                            id    = 0;
      std::int32_t                             level = -3;
      Kind                                     kind  = Kind::fancy;
      std::optional<double>                    ratio;
      std::optional<std::string>               note;
      Corner                                   corner;
      std::array<Corner, 2>                    corners{};
      std::vector<Leaf>                        leaves;
      std::variant<std::monostate, Leaf, Twig> growth;
      std::variant<Leaf, Twig>                 other;
      std::variant<std::monostate, Leaf, Twig> again;
      std::map<std::string, Leaf>              by_name;
      std::set<std::uint32_t>                  ids;
      std::vector<std::uint8_t>                blob;
      float                                    scale = 1.5f;
      bool                                     live  = true;
   };
   PSIO_REFLECT(Record, id, level, kind, ratio, note, corner, corners, leaves, growth, other, again,
                by_name, ids, blob, scale, live)
}  // namespace emit_fbs_test

namespace
{
   AnyType string_type()
   {
      return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
   }
   AnyType boolean() { return AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}}; }
   Member  field(std::string name, AnyType type, std::vector<Attribute> attrs = {})
   {
      return Member{std::move(name), Box<AnyType>{std::move(type)}, std::move(attrs)};
   }
   Schema schema_of(std::vector<Member> types)
   {
      Schema s;
      s.types = std::move(types);
      return s;
   }
   std::string error_of(const Schema& s)
   {
      try
      {
         psio::emit_fbs(s);
      }
      catch (const std::runtime_error& e)
      {
         CHECK(std::string_view{e.what()}.starts_with("emit_fbs: "));
         return e.what();
      }
      FAIL("emitted");
      return {};
   }
}  // namespace

TEST_CASE("emit_fbs: a reflected record, as the codec lays it out", "[emit_fbs]")
{
   psio::SchemaBuilder b;
   b.insert<emit_fbs_test::Record>("Record");
   auto s         = b.build();
   s.package.name = "app:data";
   CHECK(psio::emit_fbs(s) == R"(namespace app.data;

enum Record_kind : ushort {
  plain,
  fancy = 5
}

struct Corner {
  x:short;
  y:short;
}

union Record_growth {
  Leaf,
  Twig
}

union Record_other {
  Leaf,
  Twig
}

table Record_by_name {
  key:string (key);
  value:Leaf;
}

table Record {
  id:ulong;
  level:int = -3;
  kind:Record_kind = fancy;
  ratio:double = null;
  note:string;
  corner:Corner;
  corners:[Corner];
  leaves:[Leaf];
  growth:Record_growth;
  other:Record_other;
  again:Record_growth;
  by_name:[Record_by_name];
  ids:[uint];
  blob:[ubyte];
  scale:float = 1.5;
  live:bool = true;
}

table Leaf {
  name:string;
}

table Twig {
  length:uint;
}

)");
}

TEST_CASE("emit_fbs: records — table or struct", "[emit_fbs]")
{
   const auto text = psio::emit_fbs(schema_of({
      field("Fixed", AnyType{Struct{{field("a", AnyType{Int{8, false}}),
                                     field("grid", AnyType{Array{Box<AnyType>{Int{16, true}}, 3}})},
                                    {{"force_align", "8"}}}}),
      // A Struct with a member that is not fixed is a table.
      field("Loose", AnyType{Struct{{field("s", string_type())}, {}}}),
      // An Object that will not change, of fixed members: a struct.
      field("Frozen", AnyType{Object{{field("f", AnyType{Type{"Fixed"}}), field("b", boolean())},
                                     {{"definitionWillNotChange", ""}}}}),
      // … of a member that is not fixed: a table.
      field("FrozenTable", AnyType{Object{{field("s", string_type())},
                                          {{"definitionWillNotChange", ""}}}}),
      field("Open", AnyType{Object{{field("f", AnyType{Type{"Fixed"}}),
                                    field("grid", AnyType{Array{Box<AnyType>{Int{16, true}}, 3}})},
                                   {}}}),
      // One that holds itself is a table.
      field("Self", AnyType{Object{{field("next", AnyType{Option{Box<AnyType>{Type{"Self"}}}})},
                                   {{"definitionWillNotChange", ""}}}}),
   }));
   CHECK(text == R"(struct Fixed (force_align: 8) {
  a:ubyte;
  grid:[short:3];
}

table Loose {
  s:string;
}

struct Frozen {
  f:Fixed;
  b:bool;
}

table FrozenTable {
  s:string;
}

table Open {
  f:Fixed;
  grid:[short];
}

table Self {
  next:Self;
}

)");
}

TEST_CASE("emit_fbs: scalars, strings, bytes and wrappers", "[emit_fbs]")
{
   const auto text = psio::emit_fbs(schema_of({field(
      "T",
      AnyType{Object{
         {field("a", AnyType{Int{8, true}}), field("b", AnyType{Int{16, false}}),
          field("c", AnyType{Int{32, true}}), field("d", AnyType{Int{64, false}}),
          field("e", AnyType{Float{8, 24}}), field("f", AnyType{Float{11, 53}}),
          field("old_float", AnyType{Float{8, 23}}), field("g", boolean()),
          field("ch", AnyType{Custom{Box<AnyType>{Int{8, false}}, "char"}}),
          field("s", string_type()),
          field("u", AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "utf8"}}),
          field("hex", AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "hex"}}),
          field("nested", AnyType{Nested{Box<AnyType>{Int{32, false}}}}),
          field("bounded", AnyType{BoundedList{Box<AnyType>{Int{32, false}}, 8}}),
          field("when", AnyType{Custom{Box<AnyType>{Int{64, true}}, "time-point-micros"}}),
          field("set", AnyType{Custom{Box<AnyType>{List{Box<AnyType>{string_type()}}}, "set"}}),
          field("flags", AnyType{List{Box<AnyType>{boolean()}}}),
          field("maybe", AnyType{Option{Box<AnyType>{Int{16, true}}}}),
          field("maybe_list", AnyType{Option{Box<AnyType>{List{Box<AnyType>{Int{16, true}}}}}})},
         {}}})}));
   CHECK(text == R"(table T {
  a:byte;
  b:ushort;
  c:int;
  d:ulong;
  e:float;
  f:double;
  old_float:float;
  g:bool;
  ch:ubyte;
  s:string;
  u:string;
  hex:[ubyte];
  nested:[ubyte];
  bounded:[uint];
  when:long;
  set:[string];
  flags:[bool];
  maybe:short = null;
  maybe_list:[short];
}

)");
}

TEST_CASE("emit_fbs: enums", "[emit_fbs]")
{
   Schema s;
   s.types.push_back(field("Level", AnyType{Enum{Box<AnyType>{Int{8, true}},
                                                 {{"low", static_cast<std::uint64_t>(-2)},
                                                  {"mid", static_cast<std::uint64_t>(-1)},
                                                  {"zero", 0},
                                                  {"high", 7}}}}));
   s.types.push_back(field("Bits", AnyType{Enum{Box<AnyType>{Int{16, false}},
                                                {{"read", 1}, {"write", 2}, {"all", 0x8000}}}},
                           {{"bit_flags", std::nullopt}}));
   // A variant whose alternatives have no value (a WIT enum).
   s.types.push_back(field("Units", AnyType{Variant{{field("a", AnyType{Tuple{}}),
                                                     field("b", AnyType{Tuple{}})},
                                                    {}}}));
   s.types.push_back(field(
      "T", AnyType{Object{{field("l", AnyType{Type{"Level"}}, {{"default", "7"}}),
                           field("m", AnyType{Type{"Level"}}, {{"default", "-2"}}),
                           field("b", AnyType{Type{"Bits"}}, {{"default", "2"}}),
                           field("u", AnyType{Type{"Units"}}, {{"default", "1"}}),
                           field("ls", AnyType{List{Box<AnyType>{Type{"Level"}}}})},
                          {}}}));
   CHECK(psio::emit_fbs(s) == R"(enum Level : byte {
  low = -2,
  mid,
  zero,
  high = 7
}

enum Bits : ushort (bit_flags) {
  read,
  write,
  all = 15
}

enum Units : ubyte {
  a,
  b
}

table T {
  l:Level = high;
  m:Level = low;
  b:Bits = write;
  u:Units = 1;
  ls:[Level];
}

)");
}

TEST_CASE("emit_fbs: an enum without the member's default is its integer", "[emit_fbs]")
{
   // flatc refuses a field whose default (zero, when none is written) is
   // not an enumerator. The integer is the same wire.
   Schema s;
   s.types.push_back(field("NoZero", AnyType{Enum{Box<AnyType>{Int{16, true}}, {{"one", 1}, {"two", 2}}}}));
   s.types.push_back(field(
      "T", AnyType{Object{{field("unset", AnyType{Type{"NoZero"}}),
                           field("set", AnyType{Type{"NoZero"}}, {{"default", "2"}}),
                           field("other", AnyType{Type{"NoZero"}}, {{"default", "7"}}),
                           field("maybe", AnyType{Option{Box<AnyType>{Type{"NoZero"}}}}),
                           field("list", AnyType{List{Box<AnyType>{Type{"NoZero"}}}})},
                          {}}}));
   s.types.push_back(field("S", AnyType{Struct{{field("e", AnyType{Type{"NoZero"}})}, {}}}));
   CHECK(psio::emit_fbs(s) == R"(enum NoZero : short {
  one = 1,
  two
}

table T {
  unset:short;
  set:NoZero = two;
  other:short = 7;
  maybe:NoZero = null;
  list:[NoZero];
}

struct S {
  e:short;
}

)");
}

TEST_CASE("emit_fbs: defaults", "[emit_fbs]")
{
   const auto text = psio::emit_fbs(schema_of({field(
      "T", AnyType{Object{{field("a", AnyType{Int{32, true}}, {{"default", "-5"}}),
                           field("b", AnyType{Int{64, false}}, {{"default", "18446744073709551615"}}),
                           field("c", boolean(), {{"default", "true"}}),
                           field("d", boolean(), {{"default", "false"}}),
                           field("e", AnyType{Float{11, 53}}, {{"default", "0.5"}}),
                           field("f", AnyType{Float{8, 24}}, {{"default", "nan"}}),
                           field("g", AnyType{Float{11, 53}}, {{"default", "-inf"}}),
                           field("h", AnyType{Int{8, false}}, {{"default", "0"}}),
                           field("s", string_type(), {{"default", "\"hi\""}}),
                           // An optional scalar is null by default.
                           field("o", AnyType{Option{Box<AnyType>{Int{32, true}}}}, {{"default", "3"}})},
                          {}}})}));
   CHECK(text == R"(table T {
  a:int = -5;
  b:ulong = 18446744073709551615;
  c:bool = true;
  d:bool;
  e:double = 0.5;
  f:float = nan;
  g:double = -inf;
  h:ubyte;
  s:string = "hi";
  o:int = null;
}

)");
   CHECK(error_of(schema_of({field("T", AnyType{Object{{field("a", AnyType{Int{32, true}},
                                                              {{"default", "x"}})},
                                                       {}}})}))
            .find("not an integer") != std::string::npos);
}

TEST_CASE("emit_fbs: field numbers become ids only out of declaration order", "[emit_fbs]")
{
   Schema s;
   s.types.push_back(field("A", AnyType{Object{}}));
   s.types.push_back(field("U", AnyType{Variant{{field("A", AnyType{Type{"A"}})}, {}}}));
   // In order (whatever the numbers are): no ids.
   s.types.push_back(field("Ordered", AnyType{Object{{field("x", AnyType{Int{32, true}}, {{"field", "10"}}),
                                                      field("y", AnyType{Int{32, true}}, {{"field", "20"}})},
                                                     {}}}));
   // field<N>: slots in ascending order of N; a union takes two, and its
   // id is its value's.
   s.types.push_back(field("Mixed", AnyType{Object{{field("c", AnyType{Int{32, true}}, {{"field", "3"}}),
                                                    field("u", AnyType{Type{"U"}}, {{"field", "2"}}),
                                                    field("a", string_type(), {{"field", "1"}}),
                                                    field("z", AnyType{Int{8, true}})},
                                                   {}}}));
   CHECK(psio::emit_fbs(s) == R"(table A {
}

union U {
  A
}

table Ordered {
  x:int;
  y:int;
}

table Mixed {
  c:int (id: 3);
  u:U (id: 2);
  a:string (id: 0);
  z:byte (id: 4);
}

)");
}

TEST_CASE("emit_fbs: unions", "[emit_fbs]")
{
   Schema s;
   s.types.push_back(field("A", AnyType{Object{}}));
   s.types.push_back(field("B", AnyType{Object{}}));
   s.types.push_back(field("@7", AnyType{Tuple{}}));
   // A first member without a value is NONE; a label that is not the
   // type's name is an alias; explicit discriminants.
   s.types.push_back(field("U", AnyType{Variant{{field("0", AnyType{Type{"@7"}}),
                                                 field("A", AnyType{Type{"A"}}),
                                                 field("second", AnyType{Type{"A"}}),
                                                 field("B", AnyType{Type{"B"}}, {{"value", "9"}})},
                                                {}}}));
   s.types.push_back(field("T", AnyType{Object{{field("u", AnyType{Type{"U"}}),
                                                field("us", AnyType{List{Box<AnyType>{Type{"U"}}}})},
                                               {}}}));
   CHECK(psio::emit_fbs(s) == R"(table A {
}

table B {
}

union U {
  A,
  second:A,
  B = 9
}

table T {
  u:U;
  us:[U];
}

)");

   const auto bad = [](AnyType alternative) {
      Schema t;
      t.types.push_back(field("A", AnyType{Object{}}));
      t.types.push_back(field("U", AnyType{Variant{{field("A", AnyType{Type{"A"}}),
                                                    field("x", std::move(alternative))},
                                                   {}}}));
      return error_of(t);
   };
   CHECK(bad(AnyType{Int{32, false}}).find("not a table") != std::string::npos);
   CHECK(bad(AnyType{List{Box<AnyType>{Int{8, false}}}}).find("not a table") != std::string::npos);
   CHECK(bad(AnyType{Tuple{}}).find("only the first member") != std::string::npos);
}

TEST_CASE("emit_fbs: names", "[emit_fbs]")
{
   Schema s;
   s.package.name = "my:pkg";
   // A name FlatBuffers does not allow; a type of another namespace.
   s.types.push_back(field("a::Config", AnyType{Object{{field("x", AnyType{Int{32, true}})}, {}}}));
   s.types.push_back(field("With<Arg>", AnyType{Object{}}));
   s.types.push_back(field("other.ns.Far", AnyType{Object{{field("c", AnyType{Type{"a::Config"}})}, {}}}));
   s.types.push_back(field("Near", AnyType{Object{{field("f", AnyType{Type{"other.ns.Far"}}),
                                                   field("w", AnyType{Type{"With<Arg>"}})},
                                                  {}}}));
   CHECK(psio::emit_fbs(s) == R"(namespace my.pkg;

table a__Config {
  x:int;
}

table With_Arg_ {
}

namespace other.ns;

table Far {
  c:my.pkg.a__Config;
}

namespace my.pkg;

table Near {
  f:other.ns.Far;
  w:With_Arg_;
}

)");
   CHECK(error_of(schema_of({field("T", AnyType{Object{{field("not a name", AnyType{Int{8, true}})}, {}}})}))
            .find("not a field name") != std::string::npos);
}

TEST_CASE("emit_fbs: metadata, root type, identifier, includes, services", "[emit_fbs]")
{
   Schema s;
   s.package.attributes = {{"fbs_include", "common.fbs"},
                           {"fbs_file_identifier", "DATA"},
                           {"fbs_file_extension", "dat"},
                           {"fbs_root_type", "Request"}};
   s.types.push_back(field(
      "Request",
      AnyType{Object{{field("old", AnyType{Int{32, true}}, {{"deprecated", "1.2"}}),
                      field("name", string_type(), {{"required", std::nullopt}}),
                      field("id", AnyType{Int{32, false}}, {{"key", std::nullopt}}),
                      // psio's own attributes are not FlatBuffers metadata.
                      field("x", AnyType{Int{64, false}},
                            {{"jsonInt", "string"}, {"priority", "2"}, {"note", "a \"b\""}, {"hash", "fnv1_32"}})},
                     {{"maxFields", "8"}, {"audited", std::nullopt}}}}));
   s.types.push_back(field("Reply", AnyType{Object{}}));
   Interface api;
   api.name = "Api";
   api.funcs.push_back(Func{"Call", {field("request", AnyType{Type{"Request"}})},
                            Box<AnyType>{AnyType{Type{"Reply"}}}, {{"streaming", "server"}}});
   // Not an rpc method: two parameters; a scalar.
   api.funcs.push_back(Func{"Two", {field("a", AnyType{Type{"Request"}}), field("b", AnyType{Type{"Reply"}})},
                            Box<AnyType>{AnyType{Type{"Reply"}}}, {}});
   api.funcs.push_back(Func{"Scalar", {field("a", AnyType{Int{32, true}})},
                            Box<AnyType>{AnyType{Type{"Reply"}}}, {}});
   s.interfaces.push_back(api);
   CHECK(psio::emit_fbs(s) == R"(include "common.fbs";

attribute "audited";
attribute "note";
attribute "priority";

table Request (audited) {
  old:int (deprecated);
  name:string (required);
  id:uint (key);
  x:ulong (priority: 2, note: "a \"b\"", hash: "fnv1_32");
}

table Reply {
}

rpc_service Api {
  Call(Request):Reply (streaming: "server");
}

root_type Request;
file_identifier "DATA";
file_extension "dat";
)");
}

TEST_CASE("emit_fbs: what FlatBuffers does not have is an error", "[emit_fbs]")
{
   const auto with = [](AnyType type) {
      return error_of(schema_of({field("T", AnyType{Object{{field("m", std::move(type))}, {}}})}));
   };
   CHECK(with(AnyType{List{Box<AnyType>{List{Box<AnyType>{Int{8, true}}}}}}).find("vector of vectors") !=
         std::string::npos);
   CHECK(with(AnyType{Tuple{{Box<AnyType>{Int{8, true}}}}}).find("tuple") != std::string::npos);
   CHECK(with(AnyType{Tuple{}}).find("no value") != std::string::npos);
   CHECK(with(AnyType{Int{128, false}}).find("128 bits") != std::string::npos);
   CHECK(with(AnyType{Int{1, false}}).find("1 bits") != std::string::npos);
   CHECK(with(AnyType{Float{15, 113}}).find("float") != std::string::npos);
   CHECK(with(AnyType{Option{Box<AnyType>{Option{Box<AnyType>{Int{8, true}}}}}}).find("optional of an optional") !=
         std::string::npos);
   CHECK(with(AnyType{Type{"Missing"}}).find("unknown type 'Missing'") != std::string::npos);
   CHECK(with(AnyType{List{Box<AnyType>{Option{Box<AnyType>{Int{8, true}}}}}}).find("elements") !=
         std::string::npos);
   CHECK(with(AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, true}}}}, "map"}}).find("(key, value)") !=
         std::string::npos);
   Schema cyclic;
   cyclic.types.push_back(field("A", AnyType{Type{"B"}}));
   cyclic.types.push_back(field("B", AnyType{Type{"A"}}));
   cyclic.types.push_back(field("T", AnyType{Object{{field("m", AnyType{Type{"A"}})}, {}}}));
   CHECK(error_of(cyclic).find("refers to itself") != std::string::npos);
}

TEST_CASE("emit_fbs: the text reads back, and is a fixed point", "[emit_fbs]")
{
   psio::SchemaBuilder b;
   b.insert<emit_fbs_test::Record>("Record");
   const auto text   = psio::emit_fbs(b.build());
   const auto parsed = psio::parse_fbs(text);
   CHECK(psio::emit_fbs(parsed) == text);
   CHECK(psio::parse_fbs(psio::emit_fbs(parsed)) == parsed);
}
