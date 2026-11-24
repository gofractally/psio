// psio::emit_capnp — Schema IR → `.capnp` text. That `capnp compile`
// accepts the text and lays it out as psio does is capnp_schema_tests.cpp;
// that messages go both ways through the compiled schema is
// capnp_schema_interop_tests.cpp. Here: what the text says.

#include <psio/capnp_parser.hpp>
#include <psio/emit_capnp.hpp>
#include <psio/schema_builder.hpp>

#include <catch.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace psio::schema_types;

namespace emit_capnp_test
{
   enum class Kind : std::uint16_t
   {
      plain,
      fancy
   };
   PSIO_REFLECT_ENUM(Kind, plain, fancy)

   enum class Wide : std::uint32_t
   {
      a = 0,
      b = 7
   };
   PSIO_REFLECT_ENUM(Wide, a, b)

   struct Leaf
   {
      std::string name;
   };
   PSIO_REFLECT(Leaf, name)

   struct Record
   {
      std::uint64_t                                    id        = 0;
      std::int32_t                                     level     = -3;
      double                                           unit_cost = 0.5;
      bool                                             live      = true;
      Kind                                             kind      = Kind::fancy;
      Wide                                             wide      = Wide::b;
      std::optional<std::uint16_t>                     score;
      std::optional<std::string>                       note;
      std::variant<std::monostate, std::uint32_t, Leaf> pick;
      std::vector<Leaf>                                leaves;
      std::vector<std::uint8_t>                        blob;
      std::vector<std::vector<bool>>                   grid;
   };
   PSIO_REFLECT(Record, id, level, unit_cost, live, kind, wide, score, note, pick, leaves, blob, grid)

   struct Numbered
   {
      std::int32_t                             c = 0;
      std::variant<std::int8_t, std::string>   u;
      std::string                              a;
   };
   PSIO_REFLECT(Numbered, attr(c, field<3>), attr(u, field<2>), attr(a, field<1>))
}  // namespace emit_capnp_test

namespace
{
   AnyType text_type()
   {
      return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
   }
   Member field(std::string name, AnyType type, std::vector<Attribute> attrs = {})
   {
      return Member{std::move(name), Box<AnyType>{std::move(type)}, std::move(attrs)};
   }
   Schema schema_of(std::vector<Member> types)
   {
      Schema s;
      s.package.attributes.push_back({"capnp_id", "0x8000000000000001"});
      s.types = std::move(types);
      return s;
   }
   std::string error_of(const Schema& s)
   {
      try
      {
         psio::emit_capnp(s);
      }
      catch (const std::runtime_error& e)
      {
         CHECK(std::string_view{e.what()}.starts_with("emit_capnp: "));
         return e.what();
      }
      FAIL("emitted");
      return {};
   }
}  // namespace

TEST_CASE("emit_capnp: a reflected record, as the codec lays it out", "[emit_capnp]")
{
   psio::SchemaBuilder b;
   b.insert<emit_capnp_test::Record>("Record");
   b.insert<emit_capnp_test::Numbered>("Numbered");
   auto s = b.build();
   s.package.attributes.push_back({"capnp_id", "0x8000000000000001"});
   CHECK(psio::emit_capnp(s) == R"(@0x8000000000000001;

using Json = import "/capnp/compat/json.capnp";

struct Record {
  id @0 :UInt64;
  level @1 :Int32 = -3;
  unitCost @2 :Float64 = 0.5 $Json.name("unit_cost");
  live @3 :Bool = true;
  kind @4 :UInt16 = 1;
  wide @5 :UInt32 = 7;
  score :union {
    none @6 :Void;
    some @7 :UInt16;
  }
  note @8 :Text;
  pick :union {
    alt0 @9 :Void $Json.name("0");
    uint32 @10 :UInt32;
    leaf @11 :Leaf $Json.name("Leaf");
  }
  leaves @12 :List(Leaf);
  blob @13 :Data;
  grid @14 :List(List(Bool));
}

struct Leaf {
  name @0 :Text;
}

struct Numbered {
  c @3 :Int32;
  u :union {
    int8 @1 :Int8;
    string @2 :Text;
  }
  a @0 :Text;
}

)");
}

TEST_CASE("emit_capnp: enums", "[emit_capnp]")
{
   Schema s = schema_of({});
   // An enum over UInt16 with the values 0, 1, ... is a Cap'n Proto enum.
   s.types.push_back(field("Color", AnyType{Enum{Box<AnyType>{Int{16, false}},
                                                 {{"red", 0}, {"dark_blue", 1}}}}));
   // Its enumerants may be listed in another order than their ordinals.
   s.types.push_back(field("Shuffled", AnyType{Enum{Box<AnyType>{Int{16, false}},
                                                    {{"second", 1}, {"first", 0}}}}));
   // Any other enum is its integer on the wire.
   s.types.push_back(field("Sparse", AnyType{Enum{Box<AnyType>{Int{16, false}}, {{"a", 0}, {"b", 5}}}}));
   s.types.push_back(field("Narrow", AnyType{Enum{Box<AnyType>{Int{8, false}}, {{"a", 0}, {"b", 1}}}}));
   // A variant whose alternatives have no value (a WIT enum).
   s.types.push_back(field("Units", AnyType{Variant{{field("north", AnyType{Tuple{}}),
                                                     field("south", AnyType{Tuple{}})},
                                                    {}}}));
   s.types.push_back(field(
      "T", AnyType{Object{{field("c", AnyType{Type{"Color"}}, {{"default", "1"}}),
                           field("h", AnyType{Type{"Shuffled"}}, {{"default", "1"}}),
                           field("s", AnyType{Type{"Sparse"}}, {{"default", "5"}}),
                           field("n", AnyType{Type{"Narrow"}}, {{"default", "1"}}),
                           field("u", AnyType{Type{"Units"}}, {{"default", "1"}}),
                           field("cs", AnyType{List{Box<AnyType>{Type{"Color"}}}})},
                          {}}}));
   CHECK(psio::emit_capnp(s) == R"(@0x8000000000000001;

using Json = import "/capnp/compat/json.capnp";

enum Color {
  red @0;
  darkBlue @1 $Json.name("dark_blue");
}

enum Shuffled {
  second @1;
  first @0;
}

enum Units {
  north @0;
  south @1;
}

struct T {
  c @0 :Color = darkBlue;
  h @1 :Shuffled = second;
  s @2 :UInt16 = 5;
  n @3 :UInt8 = 1;
  u @4 :Units = south;
  cs @5 :List(Color);
}

)");
}

TEST_CASE("emit_capnp: unions and groups", "[emit_capnp]")
{
   const Variant shape{{field("circle", AnyType{Float{11, 53}}, {{"ordinal", "1"}}),
                        field("label", text_type(), {{"ordinal", "2"}}),
                        field("box", AnyType{Object{{field("w", AnyType{Int{32, false}}, {{"ordinal", "3"}}),
                                                     field("h", AnyType{Int{32, false}}, {{"ordinal", "4"}})},
                                                    {}}})},
                       {}};
   Schema s = schema_of({});
   s.types.push_back(field(
      "T",
      AnyType{Object{
         {field("id", AnyType{Int{64, false}}, {{"ordinal", "0"}}),
          field("", AnyType{shape}),  // the unnamed union
          field("pos", AnyType{Object{{field("x", AnyType{Int{16, true}}, {{"ordinal", "5"}}),
                                       field("y", AnyType{Int{16, true}}, {{"ordinal", "6"}})},
                                      {}}}),
          // A union with an ordinal of its own.
          field("retro", AnyType{Variant{{field("a", AnyType{Int{8, false}}, {{"ordinal", "7"}}),
                                          field("b", text_type(), {{"ordinal", "9"}})},
                                         {{"ordinal", "8"}}}}),
          field("maybe", AnyType{Option{Box<AnyType>{Int{32, true}}}}, {{"ordinal", "10"}})},
         {}}}));
   // A variant that is a named type: the struct that holds only the union.
   s.types.push_back(field("Either", AnyType{Variant{{field("left", AnyType{Int{32, true}}),
                                                      field("right", text_type())},
                                                     {}}}));
   CHECK(psio::emit_capnp(s) == R"(@0x8000000000000001;

struct T {
  id @0 :UInt64;
  union {
    circle @1 :Float64;
    label @2 :Text;
    box :group {
      w @3 :UInt32;
      h @4 :UInt32;
    }
  }
  pos :group {
    x @5 :Int16;
    y @6 :Int16;
  }
  retro @8! :union {
    a @7 :UInt8;
    b @9 :Text;
  }
  maybe :union {
    none @10 :Void;
    some @11 :Int32;
  }
}

struct Either {
  union {
    left @0 :Int32;
    right @1 :Text;
  }
}

)");
}

TEST_CASE("emit_capnp: names, nesting, ids, declarations kept as written", "[emit_capnp]")
{
   Schema s;
   s.package.name       = "my:pkg";
   s.package.attributes = {{"capnp_decl", "const limit :UInt32 = 9;"}};
   s.types.push_back(field("a::Config", AnyType{Object{{field("max_size", AnyType{Int{32, false}}),
                                                         field("Mode", AnyType{Int{8, false}}),
                                                         field("x", AnyType{Int{8, false}},
                                                               {{"capnp_name", "renamed"}, {"$note", "\"n\""}})},
                                                        {{"$note", "\"s\""},
                                                         {"capnp_decl", "const zero :Int8 = 0;"}}}},
                           {{"capnp_id", "0xdeadbeefdeadbeef"}}));
   s.types.push_back(field("a::Config.Inner", AnyType{Object{{field("v", AnyType{Type{"a::Config"}})}, {}}}));
   s.types.push_back(field("Alias", AnyType{Type{"a::Config.Inner"}}));
   s.types.push_back(field("user", AnyType{Object{{field("inner", AnyType{Type{"a::Config.Inner"}}),
                                                   field("by_alias", AnyType{Type{"Alias"}})},
                                                  {}}}));
   const auto text = psio::emit_capnp(s);
   // Without a file id one is derived from the names: the same every time.
   CHECK(text == psio::emit_capnp(s));
   CHECK(text.starts_with("@0x"));
   CHECK(text.substr(text.find('\n')) == R"(

using Json = import "/capnp/compat/json.capnp";
const limit :UInt32 = 9;

struct AConfig @0xdeadbeefdeadbeef $note("s") {
  maxSize @0 :UInt32 $Json.name("max_size");
  mode @1 :UInt8 $Json.name("Mode");
  renamed @2 :UInt8 $Json.name("x") $note("n");
  const zero :Int8 = 0;
  struct Inner {
    v @0 :AConfig;
  }
}

using Alias = AConfig.Inner;

struct User {
  inner @0 :AConfig.Inner;
  byAlias @1 :Alias $Json.name("by_alias");
}

)");
}

TEST_CASE("emit_capnp: interfaces", "[emit_capnp]")
{
   Schema s = schema_of({});
   s.types.push_back(field("Req", AnyType{Object{{field("q", text_type())}, {}}}));
   Resource r;
   r.name       = "Search";
   r.attributes = {{"capnp_extends", "Base"}};
   r.methods.push_back(Func{"find",
                            {field("query", text_type()), field("limit", AnyType{Int{32, false}}, {{"default", "10"}})},
                            Box<AnyType>{AnyType{Object{{field("hits", AnyType{List{Box<AnyType>{text_type()}}})}, {}}}},
                            {{"ordinal", "0"}}});
   r.methods.push_back(Func{"ping", {}, std::nullopt, {{"ordinal", "1"}}});
   r.methods.push_back(Func{"one", {field("x", AnyType{Int{8, false}})}, Box<AnyType>{text_type()},
                            {{"ordinal", "2"}}});
   r.methods.push_back(Func{"whole", {field("params", AnyType{Type{"Req"}})},
                            Box<AnyType>{AnyType{Type{"Req"}}},
                            {{"ordinal", "3"}, {"capnp_params", "struct"}, {"capnp_results", "struct"}}});
   r.methods.push_back(Func{"push", {field("chunk", text_type())}, std::nullopt,
                            {{"ordinal", "4"}, {"capnp_results", "stream"}}});
   s.types.push_back(field("Search", AnyType{r}));
   // A reflected interface: positional parameters; a function Cap'n Proto
   // cannot express is left out with a comment.
   Interface api;
   api.name = "api";
   api.funcs.push_back(Func{"get_one", {field("0", AnyType{Int{32, false}})}, Box<AnyType>{text_type()}, {}});
   api.funcs.push_back(Func{"bad", {field("m", AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "set"}})},
                            std::nullopt, {}});
   api.funcs.push_back(Func{"after", {}, std::nullopt, {}});
   s.interfaces.push_back(api);
   CHECK(psio::emit_capnp(s) == R"(@0x8000000000000001;

using Json = import "/capnp/compat/json.capnp";

struct Req {
  q @0 :Text;
}

interface Search extends(Base) {
  find @0 (query :Text, limit :UInt32 = 10) -> (hits :List(Text));
  ping @1 ();
  one @2 (x :UInt8) -> (result :Text);
  whole @3 Req -> Req;
  push @4 (chunk :Text) -> stream;
}

interface Api {
  getOne @0 (arg0 :UInt32) -> (result :Text) $Json.name("get_one");
  # bad: not Cap'n Proto (a set has no Cap'n Proto type)
  after @1 ();
}

)");
}

TEST_CASE("emit_capnp: what Cap'n Proto does not have is an error", "[emit_capnp]")
{
   const auto with = [](AnyType type) {
      return error_of(schema_of({field("T", AnyType{Object{{field("m", std::move(type))}, {}}})}));
   };
   CHECK(with(AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "set"}}).find("a set has no") !=
         std::string::npos);
   CHECK(with(AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Tuple{{Box<AnyType>{Int{8, false}},
                                                                    Box<AnyType>{Int{8, false}}}}}}},
                             "map"}})
            .find("a map has no") != std::string::npos);
   CHECK(with(AnyType{Tuple{{Box<AnyType>{Int{8, true}}}}}).find("tuple") != std::string::npos);
   CHECK(with(AnyType{Int{128, false}}).find("128 bits") != std::string::npos);
   CHECK(with(AnyType{Float{15, 113}}).find("float") != std::string::npos);
   CHECK(with(AnyType{Option{Box<AnyType>{Option{Box<AnyType>{Int{8, true}}}}}}).find("optional") !=
         std::string::npos);
   CHECK(with(AnyType{List{Box<AnyType>{Option{Box<AnyType>{Int{8, true}}}}}}).find("list of unions") !=
         std::string::npos);
   CHECK(with(AnyType{Variant{{field("only", AnyType{Int{8, true}})}, {}}}).find("at least two") !=
         std::string::npos);
   CHECK(with(AnyType{Type{"Missing"}}).find("unknown type 'Missing'") != std::string::npos);
   // Ordinals on some fields only; with a gap.
   CHECK(error_of(schema_of({field("T", AnyType{Object{{field("a", AnyType{Int{8, true}}, {{"ordinal", "0"}}),
                                                        field("b", AnyType{Int{8, true}})},
                                                       {}}})}))
            .find("every field has an ordinal or none") != std::string::npos);
   CHECK(error_of(schema_of({field("T", AnyType{Object{{field("a", AnyType{Int{8, true}}, {{"ordinal", "1"}})},
                                                       {}}})}))
            .find("@0 is missing") != std::string::npos);
}

TEST_CASE("emit_capnp: the text reads back, and is a fixed point", "[emit_capnp]")
{
   psio::SchemaBuilder b;
   b.insert<emit_capnp_test::Record>("Record");
   b.insert<emit_capnp_test::Numbered>("Numbered");
   const auto text   = psio::emit_capnp(b.build());
   const auto parsed = psio::parse_capnp(text);
   CHECK(psio::emit_capnp(parsed) == text);
   CHECK(psio::parse_capnp(psio::emit_capnp(parsed)) == parsed);
   // The members have their C++ names again.
   const auto& record = std::get<Object>(parsed.get("Record")->value);
   CHECK(record.members[2].name == "unit_cost");
   CHECK(*record.members[6].type == AnyType{Option{Box<AnyType>{Int{16, false}}}});
}
