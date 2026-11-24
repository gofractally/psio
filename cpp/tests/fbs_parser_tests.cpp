// psio::parse_fbs — `.fbs` text → Schema IR, and the layout of the result
// (psio::fbs_layout). The same against `flatc` is fbs_schema_tests.cpp;
// arbitrary text is schema_text_fuzz_tests.cpp. The cases are those of the
// first generation's fbs_parser tests, on the Schema IR.

#include <psio/emit_fbs.hpp>
#include <psio/fbs_layout.hpp>
#include <psio/fbs_parser.hpp>

#include <catch.hpp>

#include <string>
#include <string_view>

using namespace psio::schema_types;

namespace
{
   const std::vector<Member>& members_of(const Schema& s, std::string_view name)
   {
      const AnyType* t = s.get(name);
      REQUIRE(t != nullptr);
      if (const auto* o = std::get_if<Object>(&t->value))
         return o->members;
      REQUIRE(std::holds_alternative<Struct>(t->value));
      return std::get<Struct>(t->value).members;
   }
   const Member& member(const Schema& s, std::string_view type, std::string_view name)
   {
      for (const auto& m : members_of(s, type))
         if (m.name == name)
            return m;
      FAIL("no member " << name);
      throw 0;
   }
   std::optional<std::string> attr(const Member& m, std::string_view name)
   {
      for (const auto& a : m.attributes)
         if (a.name == name)
            return a.value ? *a.value : std::string{"<flag>"};
      return std::nullopt;
   }
   AnyType string_type()
   {
      return AnyType{Custom{Box<AnyType>{List{Box<AnyType>{Int{8, false}}}}, "string"}};
   }

   // The parse error of `text`: its message and where.
   std::string error_of(std::string_view text)
   {
      try
      {
         psio::parse_fbs(text);
      }
      catch (const psio::fbs_parse_error& e)
      {
         CHECK(e.line >= 1);
         CHECK(e.column >= 1);
         return e.what();
      }
      FAIL("accepted: " << text);
      return {};
   }
}  // namespace

TEST_CASE("parse_fbs: empty schema, namespace", "[fbs_parser]")
{
   CHECK(psio::parse_fbs("").types.empty());
   CHECK(psio::parse_fbs("  // nothing\n /* at all */ ").types.empty());
   CHECK(psio::parse_fbs("namespace MyGame.Sample;").package.name == "MyGame:Sample");
   // The package is the namespace of the first declaration.
   const auto s = psio::parse_fbs("namespace a; namespace b.c; table T {} namespace d; table U { t:b.c.T; }");
   CHECK(s.package.name == "b:c");
   CHECK(s.get("T") != nullptr);
   CHECK(s.get("d.U") != nullptr);
   CHECK(*member(s, "d.U", "t").type == AnyType{Type{"T"}});
}

TEST_CASE("parse_fbs: a table, names as written", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      table UserProfile {
         user_id:ulong;
         displayName:string;
         age:uint;
         verified:bool;
      })");
   const auto& m = members_of(s, "UserProfile");
   REQUIRE(m.size() == 4);
   CHECK(m[0].name == "user_id");
   CHECK(*m[0].type == AnyType{Int{64, false}});
   CHECK(m[1].name == "displayName");
   CHECK(*m[1].type == string_type());
   CHECK(*m[3].type == AnyType{Custom{Box<AnyType>{Int{1, false}}, "bool"}});

   const auto layouts = psio::fbs_layout(s);
   const auto* t      = layouts.find("UserProfile");
   REQUIRE(t != nullptr);
   CHECK_FALSE(t->is_struct);
   CHECK(t->slots == 4);
   CHECK(t->field("age")->slot == 2);
   CHECK(t->field("age")->vtable_offset == 8);
   CHECK(t->field("age")->size == 4);
   CHECK(t->field("displayName")->size == 4);  // an offset
}

TEST_CASE("parse_fbs: every scalar type and its alias", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      table S {
         a:bool; b:byte; c:ubyte; d:short; e:ushort; f:int; g:uint; h:long; i:ulong;
         j:float; k:double;
         b2:int8; c2:uint8; d2:int16; e2:uint16; f2:int32; g2:uint32; h2:int64; i2:uint64;
         j2:float32; k2:float64;
      })");
   const auto& m = members_of(s, "S");
   CHECK(*m[1].type == AnyType{Int{8, true}});
   CHECK(*m[2].type == AnyType{Int{8, false}});
   CHECK(*m[3].type == AnyType{Int{16, true}});
   CHECK(*m[4].type == AnyType{Int{16, false}});
   CHECK(*m[5].type == AnyType{Int{32, true}});
   CHECK(*m[6].type == AnyType{Int{32, false}});
   CHECK(*m[7].type == AnyType{Int{64, true}});
   CHECK(*m[8].type == AnyType{Int{64, false}});
   CHECK(*m[9].type == AnyType{Float{8, 24}});
   CHECK(*m[10].type == AnyType{Float{11, 53}});
   for (std::size_t i = 1; i <= 10; ++i)
      CHECK(*m[i].type == *m[i + 10].type);
   const auto  layouts = psio::fbs_layout(s);
   const auto* t       = layouts.find("S");
   const std::uint32_t sizes[] = {1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
   for (std::size_t i = 0; i < 11; ++i)
   {
      CHECK(t->fields[i].size == sizes[i]);
      CHECK(t->fields[i].align == sizes[i]);
   }
}

TEST_CASE("parse_fbs: structs, their layout and alignment", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      enum E : short { a, b }
      struct P { x:int; y:int; }
      struct S (force_align: 16) { e:E; x:ubyte; p:P; grid:[ushort:3]; d:double; ps:[P:2]; }
      struct Tail { a:ulong; b:ubyte; }
      table T { s:S; ss:[S]; })");
   CHECK(std::holds_alternative<Struct>(s.get("S")->value));
   CHECK(*member(s, "S", "grid").type == AnyType{Array{Box<AnyType>{Int{16, false}}, 3}});
   const auto layouts = psio::fbs_layout(s);
   const auto* st     = layouts.find("S");
   REQUIRE(st != nullptr);
   CHECK(st->is_struct);
   CHECK(st->field("e")->offset == 0);
   CHECK(st->field("x")->offset == 2);
   CHECK(st->field("p")->offset == 4);
   CHECK(st->field("grid")->offset == 12);
   CHECK(st->field("grid")->size == 6);
   CHECK(st->field("d")->offset == 24);
   CHECK(st->field("ps")->offset == 32);
   CHECK(st->align == 16);
   CHECK(st->size == 48);
   CHECK(layouts.find("Tail")->size == 16);  // rounded up to the alignment
   CHECK(layouts.find("Tail")->align == 8);
   // In a table a struct is inline, a vector of them an offset.
   CHECK(layouts.find("T")->field("s")->size == 48);
   CHECK(layouts.find("T")->field("s")->align == 16);
   CHECK(layouts.find("T")->field("ss")->size == 4);

   CHECK(error_of("struct S { s:string; }").find("not a scalar") != std::string::npos);
   CHECK(error_of("struct S { v:[int]; }").find("not a scalar") != std::string::npos);
   CHECK(error_of("table T {} struct S { t:T; }").find("not a scalar") != std::string::npos);
   CHECK(error_of("struct S { s:S; }").find("not a scalar") != std::string::npos);
   CHECK(error_of("struct A { b:B; } struct B { a:A; }").find("not a scalar") != std::string::npos);
   CHECK(error_of("struct S {}").find("at least one member") != std::string::npos);
   CHECK(error_of("struct S { x:int = 1; }").find("no default") != std::string::npos);
   CHECK(error_of("struct S (force_align: 3) { x:int; }").find("force_align") != std::string::npos);
   CHECK(error_of("struct S (force_align: 2) { x:int; }").find("force_align") != std::string::npos);
   CHECK(error_of("struct S { x:[int:0]; }").find("fixed array") != std::string::npos);
}

TEST_CASE("parse_fbs: enums keep their type and every value", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      enum Color : byte { Red = 0, Green, Blue = 2 }
      enum Level : ushort { low = 5, mid, high = 0x20 }
      enum Signed : int { neg = -3, zero = 0, pos }
      enum Bits : ubyte (bit_flags) { a, b, c = 7 }
      enum Untyped { x, y }
      table T { c:Color = Blue; l:Level = mid; u:Untyped; })");
   const auto& color = std::get<Enum>(s.get("Color")->value);
   CHECK(*color.type == AnyType{Int{8, true}});
   REQUIRE(color.members.size() == 3);
   CHECK(color.members[1] == Enumerator{"Green", 1});
   CHECK(color.members[2] == Enumerator{"Blue", 2});
   const auto& level = std::get<Enum>(s.get("Level")->value);
   CHECK(*level.type == AnyType{Int{16, false}});
   CHECK(level.members[0].value == 5);
   CHECK(level.members[1].value == 6);
   CHECK(level.members[2].value == 32);
   const auto& sgn = std::get<Enum>(s.get("Signed")->value);
   CHECK(sgn.members[0].value == static_cast<std::uint64_t>(std::int64_t{-3}));  // sign-extended
   CHECK(sgn.members[2].value == 1);
   const auto& bits = std::get<Enum>(s.get("Bits")->value);
   CHECK(bits.members[0].value == 1);
   CHECK(bits.members[1].value == 2);
   CHECK(bits.members[2].value == 128);
   CHECK(*std::get<Enum>(s.get("Untyped")->value).type == AnyType{Int{32, true}});

   const auto layouts = psio::fbs_layout(s);
   CHECK(layouts.find_enum("Color")->size == 1);
   CHECK(layouts.find_enum("Color")->is_signed);
   CHECK(layouts.find_enum("Level")->size == 2);
   CHECK(layouts.find("T")->field("c")->size == 1);
   CHECK(layouts.find("T")->field("l")->size == 2);
   CHECK(layouts.find("T")->field("u")->size == 4);
   // A default names an enumerator; the IR holds its value.
   CHECK(attr(member(s, "T", "c"), "default") == "2");
   CHECK(attr(member(s, "T", "l"), "default") == "6");
   CHECK(layouts.find("T")->field("c")->default_integer == 2);

   CHECK(error_of("enum E : float { a }").find("integer type") != std::string::npos);
   CHECK(error_of("enum E : byte { a = 5, b = 4 }").find("ascend") != std::string::npos);
   CHECK(error_of("enum E : byte { a = 5, b = 5 }").find("ascend") != std::string::npos);
   CHECK(error_of("enum E : ubyte { a = 256 }").find("outside") != std::string::npos);
   CHECK(error_of("enum E : byte { a = -129 }").find("outside") != std::string::npos);
   CHECK(error_of("enum E : ubyte { a = 255, b }").find("outside") != std::string::npos);
   CHECK(error_of("enum E : ubyte (bit_flags) { a = 8 }").find("bit flag") != std::string::npos);
   CHECK(error_of("enum E : byte { a, a }").find("twice") != std::string::npos);
   CHECK(error_of("enum E : byte { a } table T { e:E = b; }").find("not a value") != std::string::npos);
}

TEST_CASE("parse_fbs: unions are variants of references", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      table Sword { damage:int; }
      table Shield { armor:int; }
      union Equipment { Sword, Shield }
      union Aliased { main:Sword, off:Sword, Shield = 9 }
      table Hero { name:string; item:Equipment; hp:int; many:[Equipment]; last:Aliased; })");
   const auto& u = std::get<Variant>(s.get("Equipment")->value);
   REQUIRE(u.members.size() == 2);
   CHECK(u.members[0].name == "Sword");
   CHECK(*u.members[0].type == AnyType{Type{"Sword"}});
   const auto& a = std::get<Variant>(s.get("Aliased")->value);
   CHECK(a.members[1].name == "off");
   CHECK(*a.members[1].type == AnyType{Type{"Sword"}});
   CHECK(attr(a.members[2], "value") == "9");
   CHECK_FALSE(attr(a.members[1], "value").has_value());

   // A union takes two vtable slots: its type, then its value.
   const auto  layouts = psio::fbs_layout(s);
   const auto* hero    = layouts.find("Hero");
   CHECK(hero->field("name")->slot == 0);
   CHECK(hero->field("item")->slot == 1);
   CHECK(hero->field("item")->is_union);
   CHECK(hero->field("hp")->slot == 3);
   CHECK(hero->field("many")->slot == 4);
   CHECK(hero->field("many")->is_union);
   CHECK(hero->field("last")->slot == 6);
   CHECK(hero->slots == 8);
   const auto* e = layouts.find_enum("Aliased");
   REQUIRE(e->values.size() == 4);
   CHECK(e->values[0].name == "NONE");
   CHECK(e->value("main")->value == 1);
   CHECK(e->value("off")->value == 2);
   CHECK(e->value("Shield")->value == 9);

   CHECK(error_of("table A {} union U { A, A }").find("twice") != std::string::npos);
   CHECK(error_of("table A {} union U { NONE:A }").find("NONE") != std::string::npos);
   CHECK(error_of("table A {} union U { A = 0 }").find("ascend") != std::string::npos);
   CHECK(error_of("table A {} union U { A = 256 }").find("ascend") != std::string::npos);
   CHECK(error_of("union U { }").find("at least one") != std::string::npos);
}

TEST_CASE("parse_fbs: vectors", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      struct V { x:float; }
      table Item { name:string; }
      table T { ints:[int]; names:[string]; items:[Item]; vs:[V]; bytes:[ubyte]; one:Item; })");
   CHECK(*member(s, "T", "ints").type == AnyType{List{Box<AnyType>{Int{32, true}}}});
   CHECK(*member(s, "T", "names").type == AnyType{List{Box<AnyType>{string_type()}}});
   CHECK(*member(s, "T", "items").type == AnyType{List{Box<AnyType>{Type{"Item"}}}});
   CHECK(*member(s, "T", "bytes").type == AnyType{List{Box<AnyType>{Int{8, false}}}});
   CHECK(*member(s, "T", "one").type == AnyType{Type{"Item"}});
   const auto layouts = psio::fbs_layout(s);
   for (const auto& f : layouts.find("T")->fields)
      CHECK(f.size == 4);
   CHECK(error_of("table T { v:[[int]]; }").find("vector of vectors") != std::string::npos);
}

TEST_CASE("parse_fbs: field ids", "[fbs_parser]")
{
   // Out of declaration order: every member says its number (id + 1).
   const auto s = psio::parse_fbs(R"(
      table A {} union U { A }
      table T { c:int (id: 2); a:string (id: 0); u:U (id: 4); b:long (id: 1); })");
   CHECK(attr(member(s, "T", "c"), "field") == "3");
   CHECK(attr(member(s, "T", "a"), "field") == "1");
   CHECK(attr(member(s, "T", "u"), "field") == "5");
   const auto  layouts = psio::fbs_layout(s);
   const auto* t       = layouts.find("T");
   CHECK(t->field("a")->slot == 0);
   CHECK(t->field("b")->slot == 1);
   CHECK(t->field("c")->slot == 2);
   CHECK(t->field("u")->slot == 3);  // its type; the value is id 4
   CHECK(t->slots == 5);

   // In declaration order they say nothing: the normal form has none.
   const auto in_order = psio::parse_fbs("table T { a:int (id: 0); b:int (id: 1); }");
   CHECK(member(in_order, "T", "a").attributes.empty());
   CHECK(in_order == psio::parse_fbs("table T { a:int; b:int; }"));

   CHECK(error_of("table T { a:int (id: 0); b:int; }").find("every field") != std::string::npos);
   CHECK(error_of("table T { a:int (id: 0); b:int (id: 2); }").find("gap") != std::string::npos);
   CHECK(error_of("table T { a:int (id: 1); b:int (id: 1); }").find("twice") != std::string::npos);
   CHECK(error_of("table A {} union U { A } table T { u:U (id: 0); }").find("room") != std::string::npos);
   CHECK(error_of("table A {} union U { A } table T { u:U (id: 3); x:int (id: 0); }").find("gap") !=
         std::string::npos);
   CHECK(error_of("table T { a:int (id: x); }").find("number") != std::string::npos);
   CHECK(error_of("table T { a:int (id: 99999); }").find("number") != std::string::npos);
}

TEST_CASE("parse_fbs: defaults in their normal form", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      table T {
         a:int = 42; b:short = -7; c:uint = 0xFF; d:ubyte = 0; e:bool = true; f:bool = false;
         g:float = 1.50; h:double = -2.5e3; i:double = nan; j:float = inf; k:double = -inf;
         l:long = -0x10; m:double = 0.0; n:string = "hi"; o:[int] = [];
         p:int = null; q:bool = null; r:double = +1e2; s:float = 16777217;
      })");
   const auto value = [&](const char* name) { return attr(member(s, "T", name), "default"); };
   CHECK(value("a") == "42");
   CHECK(value("b") == "-7");
   CHECK(value("c") == "255");
   CHECK_FALSE(value("d").has_value());  // zero is no default
   CHECK(value("e") == "true");
   CHECK_FALSE(value("f").has_value());
   CHECK(value("g") == "1.5");
   CHECK(value("h") == "-2500");
   CHECK(value("i") == "nan");
   CHECK(value("j") == "inf");
   CHECK(value("k") == "-inf");
   CHECK(value("l") == "-16");
   CHECK_FALSE(value("m").has_value());
   CHECK(value("n") == "\"hi\"");
   CHECK_FALSE(value("o").has_value());
   CHECK(value("r") == "100");
   CHECK(value("s") == "16777216");  // the float the field holds
   // `= null`: an optional scalar.
   CHECK(*member(s, "T", "p").type == AnyType{Option{Box<AnyType>{Int{32, true}}}});
   CHECK_FALSE(value("p").has_value());
   const auto  layouts = psio::fbs_layout(s);
   const auto* t       = layouts.find("T");
   CHECK(t->field("a")->default_integer == 42);
   CHECK(t->field("b")->default_integer == -7);
   CHECK(t->field("e")->default_integer == 1);
   CHECK(t->field("g")->default_real == 1.5);
   CHECK(t->field("h")->default_real == -2500.0);
   CHECK(t->field("p")->optional);
   CHECK(t->field("q")->optional);
   CHECK_FALSE(t->field("a")->optional);

   CHECK(error_of("table T { a:ubyte = 256; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:ubyte = -1; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:byte = 128; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:int = abc; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:int = 1.5; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:bool = 2; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:float = x; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:string = null; }").find("only a scalar") != std::string::npos);
   CHECK(error_of("table U {} table T { a:U = 1; }").find("only a scalar") != std::string::npos);
   CHECK(error_of("table T { a:long = 99999999999999999999; }").find("not a value") != std::string::npos);
   CHECK(error_of("table T { a:long = 9223372036854775808; }").find("not a value") != std::string::npos);
   CHECK(attr(member(psio::parse_fbs("table T { a:long = -9223372036854775808; }"), "T", "a"), "default") ==
         "-9223372036854775808");
   CHECK(attr(member(psio::parse_fbs("enum E : long { lo = -9223372036854775808, hi = 9223372036854775807 }"
                                     " table T { a:E = hi; }"),
                     "T", "a"),
              "default") == "9223372036854775807");
}

TEST_CASE("parse_fbs: deprecated, required, key and other metadata", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      attribute "priority";
      attribute custom;
      table T (custom) {
         old:int (deprecated);
         name:string (required);
         id:uint (key);
         x:int (priority: 2, custom: "text", hash: "fnv1_32");
      })");
   CHECK(attr(member(s, "T", "old"), "deprecated") == "<flag>");
   CHECK(attr(member(s, "T", "name"), "required") == "<flag>");
   CHECK(attr(member(s, "T", "x"), "priority") == "2");
   CHECK(attr(member(s, "T", "x"), "custom") == "text");
   CHECK(std::get<Object>(s.get("T")->value).attributes ==
         std::vector<Attribute>{{"custom", std::nullopt}});
   const auto  layouts = psio::fbs_layout(s);
   const auto* t       = layouts.find("T");
   CHECK(t->field("old")->deprecated);
   CHECK(t->field("old")->slot == 0);  // it keeps its slot
   CHECK(t->field("name")->required);
   CHECK(t->field("id")->key);
   CHECK_FALSE(t->field("id")->required);
   // A string key is required (flatc's rule).
   CHECK(psio::fbs_layout(psio::parse_fbs("table E { k:string (key); }")).find("E")->field("k")->required);

   // Names the IR gives a meaning of its own are not metadata.
   CHECK(error_of("table T { x:int (field: 3); }").find("reserved") != std::string::npos);
   CHECK(error_of("table T (definitionWillNotChange) { x:int; }").find("reserved") != std::string::npos);
   CHECK(error_of("table T { x:int (a, a); }").find("twice") != std::string::npos);
}

TEST_CASE("parse_fbs: root type, identifier, extension, includes, services", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      include "common.fbs";
      include "more.fbs";
      namespace game;
      table Monster { hp:int; }
      table Reply { ok:bool; }
      rpc_service Arena { Fight(Monster):Reply; Watch(Monster):Reply (streaming: "server"); }
      root_type Monster;
      file_identifier "MONS";
      file_extension "mon";)");
   const auto value = [&](const char* name) {
      std::vector<std::string> out;
      for (const auto& a : s.package.attributes)
         if (a.name == name)
            out.push_back(*a.value);
      return out;
   };
   CHECK(value("fbs_root_type") == std::vector<std::string>{"Monster"});
   CHECK(value("fbs_file_identifier") == std::vector<std::string>{"MONS"});
   CHECK(value("fbs_file_extension") == std::vector<std::string>{"mon"});
   CHECK(value("fbs_include") == std::vector<std::string>{"common.fbs", "more.fbs"});
   REQUIRE(s.interfaces.size() == 1);
   CHECK(s.interfaces[0].name == "Arena");
   REQUIRE(s.interfaces[0].funcs.size() == 2);
   CHECK(s.interfaces[0].funcs[0].name == "Fight");
   CHECK(*s.interfaces[0].funcs[0].params[0].type == AnyType{Type{"Monster"}});
   CHECK(**s.interfaces[0].funcs[0].result == AnyType{Type{"Reply"}});
   const auto layouts = psio::fbs_layout(s);
   CHECK(layouts.root_type == "Monster");
   CHECK(layouts.file_identifier == "MONS");

   CHECK(error_of("root_type Missing;").find("root_type") != std::string::npos);
   CHECK(error_of("struct S { x:int; } root_type S;").find("root_type") != std::string::npos);
   CHECK(error_of("file_identifier \"TOOLONG\";").find("4 characters") != std::string::npos);
   // A type an included file declares is kept by name; the layout needs it.
   const auto partial = psio::parse_fbs("include \"x.fbs\"; table T { other:Elsewhere; }");
   CHECK(*member(partial, "T", "other").type == AnyType{Type{"Elsewhere"}});
   CHECK_THROWS_WITH(psio::fbs_layout(partial), Catch::Contains("unknown type 'Elsewhere'"));
}

TEST_CASE("parse_fbs: names are looked up through the enclosing namespaces", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      namespace a.b;
      table Inner { x:int; }
      enum E : byte { p, q }
      namespace a.b.c;
      table Leaf { i:Inner; e:E = q; full:a.b.Inner; }
      namespace z;
      table Inner { y:int; }
      table Uses { mine:Inner; theirs:a.b.Inner; leaf:a.b.c.Leaf; })");
   CHECK(s.package.name == "a:b");
   CHECK(*member(s, "a.b.c.Leaf", "i").type == AnyType{Type{"Inner"}});
   CHECK(*member(s, "a.b.c.Leaf", "full").type == AnyType{Type{"Inner"}});
   CHECK(attr(member(s, "a.b.c.Leaf", "e"), "default") == "1");
   CHECK(*member(s, "z.Uses", "mine").type == AnyType{Type{"z.Inner"}});
   CHECK(*member(s, "z.Uses", "theirs").type == AnyType{Type{"Inner"}});
   CHECK(*member(s, "z.Uses", "leaf").type == AnyType{Type{"a.b.c.Leaf"}});
   CHECK(error_of("table T {} table T {}").find("twice") != std::string::npos);
   CHECK(error_of("table T { a:int; a:int; }").find("twice") != std::string::npos);
}

TEST_CASE("parse_fbs: the Monster example", "[fbs_parser]")
{
   const auto s = psio::parse_fbs(R"(
      namespace MyGame.Sample;
      enum Color : byte { Red = 0, Green, Blue = 2 }
      union Equipment { Weapon }
      struct Vec3 { x:float; y:float; z:float; }
      table Monster {
         pos:Vec3; mana:short = 150; hp:short = 100; name:string; friendly:bool = false;
         inventory:[ubyte]; color:Color = Blue; weapons:[Weapon]; equipped:Equipment; path:[Vec3];
      }
      table Weapon { name:string; damage:short; }
      root_type Monster;)");
   CHECK(s.package.name == "MyGame:Sample");
   const auto  layouts = psio::fbs_layout(s);
   CHECK(layouts.root_type == "Monster");
   const auto* vec3 = layouts.find("Vec3");
   CHECK(vec3->is_struct);
   CHECK(vec3->size == 12);
   CHECK(vec3->align == 4);
   const auto* monster = layouts.find("Monster");
   REQUIRE(monster->fields.size() == 10);
   CHECK(monster->field("pos")->slot == 0);
   CHECK(monster->field("pos")->size == 12);
   CHECK(monster->field("mana")->default_integer == 150);
   CHECK(monster->field("hp")->default_integer == 100);
   CHECK(monster->field("color")->slot == 6);
   CHECK(monster->field("color")->size == 1);
   CHECK(monster->field("color")->default_integer == 2);
   CHECK(monster->field("equipped")->slot == 8);
   CHECK(monster->field("equipped")->is_union);
   CHECK(monster->field("path")->slot == 10);
   CHECK(monster->slots == 11);
   CHECK(layouts.find_enum("Equipment")->value("Weapon")->value == 1);
}

TEST_CASE("parse_fbs: malformed text is an error with a position", "[fbs_parser]")
{
   try
   {
      psio::parse_fbs("table T {\n  a:int;\n  b int;\n}");
      FAIL("accepted");
   }
   catch (const psio::fbs_parse_error& e)
   {
      CHECK(e.line == 3);
      CHECK(e.column == 5);
      CHECK(std::string_view{e.what()}.starts_with("fbs:3:5: "));
   }
   for (const char* text :
        {"this is not a schema", "table", "table T", "table T {", "table T { a }", "table T { a: }",
         "table T { a:int }", "table T { a:int; ", "table T { a:[int; }", "table T { a:int = ; }",
         "table T { a:int (; }", "table T { a:int (id:); }", "namespace ;", "namespace a.;",
         "enum E : byte { a = }", "enum E : byte { a b }", "union U { A", "root_type ;",
         "file_identifier ABCD;", "table T { s:string = \"unterminated; }", "/* open", "{ \"a\": 1 }",
         "rpc_service S { M(A) }", "rpc_service S { M(A):B }", "table T { a:int; } }", "@", "table 9 {}",
         "table T { a:int = \"\\q\"; }", "include x;", "attribute 3;"})
   {
      INFO(text);
      CHECK_THROWS_AS(psio::parse_fbs(text), psio::fbs_parse_error);
   }
}
