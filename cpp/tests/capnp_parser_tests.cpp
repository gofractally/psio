// psio::parse_capnp — `.capnp` text → Schema IR, and the layout of the
// result (psio::capnp_layout). The same against `capnp compile` is
// capnp_schema_tests.cpp; arbitrary text is schema_text_fuzz_tests.cpp. The
// cases are those of the first generation's capnp_parser tests, on the
// Schema IR and with the official layout (the first generation gave every
// union member its own slot).

#include <psio/capnp_layout.hpp>
#include <psio/capnp_parser.hpp>
#include <psio/emit_capnp.hpp>

#include <catch.hpp>

#include <bit>
#include <string>
#include <string_view>

using namespace psio::schema_types;

namespace
{
   const std::vector<Member>& members_of(const Schema& s, std::string_view name)
   {
      const AnyType* t = s.get(name);
      REQUIRE(t != nullptr);
      REQUIRE(std::holds_alternative<Object>(t->value));
      return std::get<Object>(t->value).members;
   }
   const Member& member(const std::vector<Member>& members, std::string_view name)
   {
      for (const auto& m : members)
         if (m.name == name)
            return m;
      FAIL("no member " << name);
      throw 0;
   }
   const Member& member(const Schema& s, std::string_view type, std::string_view name)
   {
      return member(members_of(s, type), name);
   }
   std::optional<std::string> attr(const std::vector<Attribute>& attrs, std::string_view name)
   {
      for (const auto& a : attrs)
         if (a.name == name)
            return a.value ? *a.value : std::string{"<flag>"};
      return std::nullopt;
   }
   std::optional<std::string> attr(const Member& m, std::string_view name)
   {
      return attr(m.attributes, name);
   }
   AnyType text_type()
   {
      return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
   }
   AnyType data_type()
   {
      return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "hex"}};
   }

   std::string error_of(std::string_view text)
   {
      try
      {
         psio::parse_capnp(text);
      }
      catch (const psio::capnp_parse_error& e)
      {
         CHECK(e.line >= 1);
         CHECK(e.column >= 1);
         return e.what();
      }
      FAIL("accepted: " << text);
      return {};
   }

   // A data field: its size in bits and where it starts, in bits.
   void data_at(const capnp_scope_layout& scope, const char* name, unsigned bits, unsigned bit)
   {
      INFO(name);
      const auto* f = scope.field(name);
      REQUIRE(f != nullptr);
      REQUIRE(f->kind == capnp_slot_kind::data);
      CHECK((1u << f->lg) == bits);
      CHECK(f->bit_offset() == bit);
   }
   void pointer_at(const capnp_scope_layout& scope, const char* name, unsigned index)
   {
      INFO(name);
      const auto* f = scope.field(name);
      REQUIRE(f != nullptr);
      REQUIRE(f->kind == capnp_slot_kind::pointer);
      CHECK(f->offset == index);
   }
}  // namespace

TEST_CASE("parse_capnp: file id, comments, an empty file", "[capnp_parser]")
{
   CHECK(psio::parse_capnp("").types.empty());
   CHECK(psio::parse_capnp("# only a comment\n").types.empty());
   const auto s = psio::parse_capnp("@0xdbb9ad1f14bf0b36;  # the id\n# nothing else\n");
   CHECK(attr(s.package.attributes, "capnp_id") == "0xdbb9ad1f14bf0b36");
   CHECK(error_of("@0x1234;").find("first bit set") != std::string::npos);
   CHECK(error_of("@12;").find("hexadecimal") != std::string::npos);
}

TEST_CASE("parse_capnp: a struct of scalars and pointers, and its layout", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      @0xdbb9ad1f14bf0b36;
      struct User @0xf8e1d2c3b4a59687 {
         id @0 :UInt64;        # a comment after a field
         name @1 :Text;
         email @2 :Text;
         age @3 :UInt32;
         active @4 :Bool;
         score @5 :Float64;
         tags @6 :List(Text);
         avatar @7 :Data;
      })");
   const auto& m = members_of(s, "User");
   REQUIRE(m.size() == 8);
   CHECK(*m[0].type == AnyType{Int{64, false}});
   CHECK(*m[1].type == text_type());
   CHECK(*m[4].type == AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}});
   CHECK(*m[5].type == AnyType{Float{11, 53}});
   CHECK(*m[6].type == AnyType{List{Box<AnyType>{text_type()}}});
   CHECK(*m[7].type == data_type());
   CHECK(attr(m[3], "ordinal") == "3");
   CHECK(attr(s.types[0].attributes, "capnp_id") == "0xf8e1d2c3b4a59687");

   const auto  layouts = psio::capnp_layout(s);
   const auto* user    = layouts.find("User");
   REQUIRE(user != nullptr);
   CHECK(user->data_words == 3);
   CHECK(user->pointers == 4);
   data_at(*user, "id", 64, 0);
   data_at(*user, "age", 32, 64);
   data_at(*user, "active", 1, 96);
   data_at(*user, "score", 64, 128);
   pointer_at(*user, "name", 0);
   pointer_at(*user, "email", 1);
   pointer_at(*user, "tags", 2);
   pointer_at(*user, "avatar", 3);
}

TEST_CASE("parse_capnp: every builtin type", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      struct All {
         v @0 :Void; b @1 :Bool; i8 @2 :Int8; i16 @3 :Int16; i32 @4 :Int32; i64 @5 :Int64;
         u8 @6 :UInt8; u16 @7 :UInt16; u32 @8 :UInt32; u64 @9 :UInt64; f32 @10 :Float32;
         f64 @11 :Float64; t @12 :Text; d @13 :Data; p @14 :AnyPointer;
         l @15 :List(List(UInt8)); s @16 :AnyStruct; c @17 :Capability;
      })");
   const auto& m = members_of(s, "All");
   CHECK(*m[0].type == AnyType{Tuple{}});
   CHECK(*m[2].type == AnyType{Int{8, true}});
   CHECK(*m[9].type == AnyType{Int{64, false}});
   CHECK(*m[10].type == AnyType{Float{8, 24}});
   CHECK(*m[14].type == AnyType{Custom{Box<AnyType>{Tuple{}}, "AnyPointer"}});
   CHECK(*m[15].type ==
         AnyType{List{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}}});
   // The official layout: fields in ordinal order, each in the first hole
   // that fits.
   const auto  layouts = psio::capnp_layout(s);
   const auto* all     = layouts.find("All");
   CHECK(all->data_words == 6);
   CHECK(all->pointers == 6);
   CHECK(all->field("v")->kind == capnp_slot_kind::none);
   data_at(*all, "b", 1, 0);
   data_at(*all, "i8", 8, 8);
   data_at(*all, "i16", 16, 16);
   data_at(*all, "i32", 32, 32);
   data_at(*all, "i64", 64, 64);
   data_at(*all, "u8", 8, 128);
   data_at(*all, "u16", 16, 144);
   data_at(*all, "u32", 32, 160);
   data_at(*all, "u64", 64, 192);
   data_at(*all, "f32", 32, 256);
   data_at(*all, "f64", 64, 320);
}

TEST_CASE("parse_capnp: enums are enums over UInt16", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      enum Color { red @0; green @1; blue @2; }
      enum Shuffled @0xa1b2c3d4e5f60718 { second @1; first @0; }
      struct Pixel { before @0 :UInt8; color @1 :Color = blue; other @2 :Shuffled = second; })");
   const auto& color = std::get<Enum>(s.get("Color")->value);
   CHECK(*color.type == AnyType{Int{16, false}});
   REQUIRE(color.members.size() == 3);
   CHECK(color.members[2] == Enumerator{"blue", 2});
   const auto& shuffled = std::get<Enum>(s.get("Shuffled")->value);
   CHECK(shuffled.members[0] == Enumerator{"second", 1});
   CHECK(shuffled.members[1] == Enumerator{"first", 0});
   CHECK(*member(s, "Pixel", "color").type == AnyType{Type{"Color"}});
   // A default names an enumerant; the IR holds its value.
   CHECK(attr(member(s, "Pixel", "color"), "default") == "2");
   CHECK(attr(member(s, "Pixel", "other"), "default") == "1");
   const auto  layouts = psio::capnp_layout(s);
   const auto* pixel   = layouts.find("Pixel");
   data_at(*pixel, "color", 16, 16);
   CHECK(pixel->field("color")->default_bits == 2);

   CHECK(error_of("enum E { a @0; b @2; }").find("gap") != std::string::npos);
   CHECK(error_of("enum E { a @0; b @0; }").find("twice") != std::string::npos);
   CHECK(error_of("enum E { a; }").find("ordinal") != std::string::npos);
   CHECK(error_of("enum E { a @0; } struct S { e @0 :E = b; }").find("not a value") !=
         std::string::npos);
}

TEST_CASE("parse_capnp: unions, with the official layout", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      struct Shape {
         area @0 :Float64;
         union {
            circle @1 :Float64;
            square @2 :Float32;
            label @3 :Text;
            nothing @4 :Void;
         }
         named :union { a @5 :UInt8; b @6 :Text; }
      })");
   const auto& m = members_of(s, "Shape");
   REQUIRE(m.size() == 3);
   CHECK(m[1].name.empty());  // the unnamed union
   const auto& u = std::get<Variant>(m[1].type->value);
   REQUIRE(u.members.size() == 4);
   CHECK(u.members[0].name == "circle");
   CHECK(attr(u.members[2], "ordinal") == "3");
   CHECK(*u.members[3].type == AnyType{Tuple{}});
   CHECK(std::holds_alternative<Variant>(m[2].type->value));

   const auto  layouts = psio::capnp_layout(s);
   const auto* shape   = layouts.find("Shape");
   CHECK(shape->data_words == 3);
   CHECK(shape->pointers == 2);
   // The members of the unnamed union are the struct's: they share space.
   CHECK(shape->discriminant_count == 4);
   CHECK(shape->discriminant_offset == 8);  // in 16-bit units: bits 128..144
   data_at(*shape, "circle", 64, 64);
   data_at(*shape, "square", 32, 64);
   pointer_at(*shape, "label", 0);
   CHECK(*shape->field("circle")->discriminant == 0);
   CHECK(*shape->field("nothing")->discriminant == 3);
   CHECK_FALSE(shape->field("area")->discriminant.has_value());
   // A named union is a group.
   const auto* named = shape->field("named");
   REQUIRE(named->kind == capnp_slot_kind::group);
   CHECK(named->group[0].discriminant_count == 2);
   data_at(named->group[0], "a", 8, 144);
   pointer_at(named->group[0], "b", 1);  // (each union has its own locations)
}

TEST_CASE("parse_capnp: groups, an optional scalar, a union's own ordinal", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      struct T {
         head @0 :UInt8;
         pos :group { x @1 :Int32; y @2 :Int32; }
         score :union { none @3 :Void; some @4 :UInt32; }
         retro @6! :union { first @5 :UInt16; second @7 :Text; }
         choice :union {
            small @8 :UInt8;
            big :group { a @9 :UInt64; b @10 :Text; }
         }
      })");
   const auto& m = members_of(s, "T");
   // A group is an Object written in place.
   const auto& pos = std::get<Object>(member(m, "pos").type->value);
   CHECK(pos.members.size() == 2);
   // { none :Void; some :T } of a data type is the optional.
   CHECK(*member(m, "score").type == AnyType{Option{Box<AnyType>{Int{32, false}}}});
   CHECK(attr(member(m, "score"), "ordinal") == "3");
   CHECK(attr(std::get<Variant>(member(m, "retro").type->value).attributes, "ordinal") == "6");

   const auto  layouts = psio::capnp_layout(s);
   const auto* t       = layouts.find("T");
   const auto& group   = t->field("pos")->group.at(0);
   data_at(group, "x", 32, 32);
   data_at(group, "y", 32, 64);
   const auto& score = t->field("score")->group.at(0);
   CHECK(score.discriminant_count == 2);
   CHECK(score.fields[0].kind == capnp_slot_kind::none);
   CHECK(*score.fields[1].ordinal == 4);
   // The discriminant of `retro` was added at @6, after its first member.
   const auto& retro = t->field("retro")->group.at(0);
   CHECK(*t->field("retro")->ordinal == 6);
   CHECK(retro.fields[0].bit_offset() + 16 <= retro.discriminant_offset * 16 + 16);
   // A group in a union: its members are one member of the union.
   const auto& choice = t->field("choice")->group.at(0);
   CHECK(choice.discriminant_count == 2);
   CHECK(*choice.field("big")->discriminant == 1);
   CHECK(choice.field("big")->group.at(0).fields.size() == 2);

   CHECK(error_of("struct S { g :group { } }").find("at least one member") != std::string::npos);
   CHECK(error_of("struct S { u :union { a @0 :Void; } }").find("at least two") != std::string::npos);
   CHECK(error_of("struct S { union { a @0 :Void; union { b @1 :Void; c @2 :Void; } } }")
            .find("cannot hold a union") != std::string::npos);
   CHECK(error_of("struct S { union { a @0 :Void; b @1 :Void; } union { c @2 :Void; d @3 :Void; } }")
            .find("one unnamed union") != std::string::npos);
   CHECK(error_of("struct S { union @0 { a @1 :Void; b @2 :Void; } }").find("no ordinal") !=
         std::string::npos);
   CHECK(error_of("struct S { g @0 :group { a @1 :Void; } }").find("no ordinal") != std::string::npos);
}

TEST_CASE("parse_capnp: ordinals", "[capnp_parser]")
{
   // The discriminant values follow the ordinals, not the text.
   const auto s = psio::parse_capnp(R"(
      struct Evolved {
         c @3 :UInt8;
         a @0 :UInt64;
         choice :union { late @4 :Text; first @1 :UInt16; second @2 :UInt32; }
      })");
   const auto  layouts = psio::capnp_layout(s);
   const auto& choice  = layouts.find("Evolved")->field("choice")->group.at(0);
   CHECK(*choice.field("first")->discriminant == 0);
   CHECK(*choice.field("second")->discriminant == 1);
   CHECK(*choice.field("late")->discriminant == 2);
   data_at(*layouts.find("Evolved"), "a", 64, 0);

   CHECK(error_of("struct S { a @0 :Bool; b @2 :Bool; }").find("@1 is missing") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Bool; b @0 :Bool; }").find("used twice") != std::string::npos);
   CHECK(error_of("struct S { a @1 :Bool; }").find("@0 is missing") != std::string::npos);
   CHECK(error_of("struct S { a :Bool; }").find("has an ordinal") != std::string::npos);
   CHECK(error_of("struct S { a @70000 :Bool; }").find("below 65536") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Bool; a @1 :Bool; }").find("twice") != std::string::npos);
}

TEST_CASE("parse_capnp: defaults in their normal form", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      struct Inner { a @0 :Int32; }
      struct D {
         a @0 :Int32 = -5; b @1 :UInt8 = 0xff; c @2 :Bool = true; d @3 :Bool = false;
         e @4 :Float64 = 2.50; f @5 :Float32 = 1e3; g @6 :Float64 = inf; h @7 :Float64 = -inf;
         i @8 :Float32 = nan; j @9 :UInt64 = 18446744073709551615; k @10 :Int64 = 0;
         name @11 :Text = "anon"; inner @12 :Inner = (a = 3); list @13 :List(Int8) = [1, -2];
         bytes @14 :Data = 0x"00 ff"; none @15 :Void = void;
      })");
   const auto value = [&](const char* name) { return attr(member(s, "D", name), "default"); };
   CHECK(value("a") == "-5");
   CHECK(value("b") == "255");
   CHECK(value("c") == "true");
   CHECK_FALSE(value("d").has_value());  // zero is no default
   CHECK(value("e") == "2.5");
   CHECK(value("f") == "1000");
   CHECK(value("g") == "inf");
   CHECK(value("h") == "-inf");
   CHECK(value("i") == "nan");
   CHECK(value("j") == "18446744073709551615");
   CHECK_FALSE(value("k").has_value());
   // A pointer's default is kept as written.
   CHECK(value("name") == "\"anon\"");
   CHECK(value("inner") == "(a = 3)");
   CHECK(value("list") == "[1, -2]");
   CHECK(value("bytes") == "0x\"00 ff\"");
   CHECK_FALSE(value("none").has_value());
   // The wire holds value XOR default.
   const auto  layouts = psio::capnp_layout(s);
   const auto* d       = layouts.find("D");
   CHECK(d->field("a")->default_bits == 0xfffffffbu);
   CHECK(d->field("b")->default_bits == 0xff);
   CHECK(d->field("c")->default_bits == 1);
   CHECK(d->field("e")->default_bits == std::bit_cast<std::uint64_t>(2.5));
   CHECK(d->field("f")->default_bits == std::bit_cast<std::uint32_t>(1000.0f));
   CHECK(d->field("j")->default_bits == ~std::uint64_t{0});

   CHECK(error_of("struct S { a @0 :UInt8 = 256; }").find("not a value") != std::string::npos);
   CHECK(error_of("struct S { a @0 :UInt8 = -1; }").find("not a value") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Int8 = 128; }").find("not a value") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Bool = 1; }").find("not a value") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Int32 = x; }").find("not a value") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Float32 = x; }").find("not a value") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Int32 = ; }").find("default value") != std::string::npos);
}

TEST_CASE("parse_capnp: nested declarations and name lookup", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      struct Outer {
         inner @0 :Inner;
         struct Inner {
            v @0 :Int32;
            enum Kind { a @0; b @1; }
            kind @1 :Kind;
            struct Deepest { back @0 :Outer; up @1 :Inner; }
         }
         list @1 :List(Inner.Deepest);
         top @2 :Top;
         kind @3 :Inner.Kind = b;
      }
      struct Top { o @0 :Outer.Inner; before @1 :Later; }
      struct Later { x @0 :Bool; })");
   CHECK(s.get("Outer.Inner") != nullptr);
   CHECK(s.get("Outer.Inner.Kind") != nullptr);
   CHECK(s.get("Outer.Inner.Deepest") != nullptr);
   CHECK(*member(s, "Outer", "inner").type == AnyType{Type{"Outer.Inner"}});
   CHECK(*member(s, "Outer.Inner", "kind").type == AnyType{Type{"Outer.Inner.Kind"}});
   CHECK(*member(s, "Outer.Inner.Deepest", "back").type == AnyType{Type{"Outer"}});
   CHECK(*member(s, "Outer.Inner.Deepest", "up").type == AnyType{Type{"Outer.Inner"}});
   CHECK(*member(s, "Outer", "list").type ==
         AnyType{List{Box<AnyType>{Type{"Outer.Inner.Deepest"}}}});
   CHECK(*member(s, "Top", "o").type == AnyType{Type{"Outer.Inner"}});
   CHECK(*member(s, "Top", "before").type == AnyType{Type{"Later"}});  // used before declared
   CHECK(attr(member(s, "Outer", "kind"), "default") == "1");
   CHECK(error_of("struct S { a @0 :Bool; } struct S { b @0 :Bool; }").find("twice") != std::string::npos);
   // A type of an imported file is kept by name; the layout needs it.
   const auto partial = psio::parse_capnp("struct S { other @0 :Elsewhere; }");
   CHECK_THROWS_WITH(psio::capnp_layout(partial), Catch::Contains("unknown type 'Elsewhere'"));
}

TEST_CASE("parse_capnp: using, const, annotation declarations", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      @0xdbb9ad1f14bf0b36;
      using Json = import "/capnp/compat/json.capnp";
      using Cxx = import "/capnp/c++.capnp";
      $Cxx.namespace("demo");
      using Id = UInt64;
      using P = Point;
      const limit :UInt32 = 100;
      const origin :Point = (x = 1, y = 2);
      annotation tag(field, struct) :Text;
      struct Point $tag("a point") {
         x @0 :Int32 $tag("first") $Json.name("x_coordinate");
         y @1 :Int32;
         id @2 :Id;
         next @3 :P;
         const zero :Int32 = 0;
      })");
   // What psio does not model is kept as written …
   std::vector<std::string> kept;
   for (const auto& a : s.package.attributes)
      if (a.name == "capnp_decl")
         kept.push_back(*a.value);
   REQUIRE(kept.size() == 6);
   CHECK(kept[0] == "using Json = import \"/capnp/compat/json.capnp\";");
   CHECK(kept[2] == "$Cxx.namespace(\"demo\");");
   CHECK(kept[3] == "const limit :UInt32 = 100;");
   CHECK(kept[4] == "const origin :Point = (x = 1, y = 2);");
   CHECK(kept[5] == "annotation tag(field, struct) :Text;");
   const auto& point = std::get<Object>(s.get("Point")->value);
   CHECK(attr(point.attributes, "$tag") == "\"a point\"");
   CHECK(attr(point.attributes, "capnp_decl") == "const zero :Int32 = 0;");
   // … an alias is a named type …
   CHECK(*s.get("Id") == AnyType{Int{64, false}});
   CHECK(*s.get("P") == AnyType{Type{"Point"}});
   const auto layouts = psio::capnp_layout(s);
   data_at(*layouts.find("Point"), "id", 64, 64);
   pointer_at(*layouts.find("Point"), "next", 0);
   // … and the JSON name is the member's name; Cap'n Proto's own is kept
   // when it is not the camelCase of it.
   const auto& x = point.members[0];
   CHECK(x.name == "x_coordinate");
   CHECK(attr(x, "capnp_name") == "x");
   CHECK(attr(x, "$tag") == "\"first\"");
   const auto snake = psio::parse_capnp(
      "using J = import \"/capnp/compat/json.capnp\";\n"
      "struct S { unitPrice @0 :Float64 $J.name(\"unit_price\"); }");
   CHECK(members_of(snake, "S")[0].name == "unit_price");
   CHECK_FALSE(attr(members_of(snake, "S")[0], "capnp_name").has_value());
}

TEST_CASE("parse_capnp: interfaces", "[capnp_parser]")
{
   const auto s = psio::parse_capnp(R"(
      struct Req { q @0 :Text; }
      interface Search @0xb1c2d3e4f5a69788 {
         find @0 (query :Text, limit :UInt32 = 10) -> (hits :List(Text), total :UInt64);
         ping @1 ();
         one @2 (x :UInt8) -> (result :Text);
         whole @3 Req -> Req;
         push @4 (chunk :Data) -> stream;
         struct Options { verbose @0 :Bool; }
      }
      interface More extends(Search) { extra @0 (); }
      struct Holder { s @0 :Search; })");
   const auto& search = std::get<Resource>(s.get("Search")->value);
   REQUIRE(search.methods.size() == 5);
   const auto& find = search.methods[0];
   CHECK(find.name == "find");
   CHECK(attr(find.attributes, "ordinal") == "0");
   REQUIRE(find.params.size() == 2);
   CHECK(attr(find.params[1], "default") == "10");
   const auto& results = std::get<Object>((*find.result)->value);
   CHECK(results.members.size() == 2);
   CHECK_FALSE(search.methods[1].result.has_value());
   CHECK(**search.methods[2].result == text_type());  // one result named `result`
   CHECK(attr(search.methods[3].attributes, "capnp_params") == "struct");
   CHECK(**search.methods[3].result == AnyType{Type{"Req"}});
   CHECK(attr(search.methods[4].attributes, "capnp_results") == "stream");
   CHECK(s.get("Search.Options") != nullptr);
   CHECK(attr(std::get<Resource>(s.get("More")->value).attributes, "capnp_extends") == "Search");
   // A capability is a pointer.
   const auto layouts = psio::capnp_layout(s);
   pointer_at(*layouts.find("Holder"), "s", 0);
   CHECK(error_of("interface I { m (); }").find("ordinal") != std::string::npos);
   CHECK(error_of("interface I { m @0 [T] (a :T); }").find("generic") != std::string::npos);
}

TEST_CASE("parse_capnp: what is not read is refused", "[capnp_parser]")
{
   CHECK(error_of("struct Box(T) { v @0 :T; }").find("generic") != std::string::npos);
   CHECK(error_of("struct S { m @0 :Map(Text, Text); }").find("generic") != std::string::npos);
   CHECK(error_of("struct S { a @0 :Text $import \"/x.capnp\".y; }").find("import") !=
         std::string::npos);
}

TEST_CASE("parse_capnp: malformed text is an error with a position", "[capnp_parser]")
{
   try
   {
      psio::parse_capnp("struct S {\n  a @0 :Bool;\n  b @1 Bool;\n}");
      FAIL("accepted");
   }
   catch (const psio::capnp_parse_error& e)
   {
      CHECK(e.line == 3);
      CHECK(e.column == 8);
      CHECK(std::string_view{e.what()}.starts_with("capnp:3:8: "));
   }
   for (const char* text :
        {"this is not a schema", "struct", "struct S", "struct S {", "struct S { a }",
         "struct S { a @0 }", "struct S { a @0 : }", "struct S { a @0 :Bool }", "struct S { a @0 :Bool; ",
         "struct S { a @0 :List(; }", "struct S { a @0 :List(Bool; }", "struct S { a @x :Bool; }",
         "enum E {", "enum E { a @0 }", "interface I {", "interface I { m @0 (a) ; }",
         "interface I { m @0 (a :Bool ; }", "using X = ;", "using X", "const c :Int32 = (1;",
         "@0xdbb9ad1f14bf0b36", "struct S { a @0 :Text = \"open; }", "struct S { a @0 :Bool = (; }",
         "struct S { } }", "$", "struct 9 {}", "struct S { a @0 :Bool $x(; }", "annotation a(",
         "struct S { union { a @0 :Bool; } }"})
   {
      INFO(text);
      CHECK_THROWS_AS(psio::parse_capnp(text), psio::capnp_parse_error);
   }
}
