// Tests for psio/schema_ir.hpp — Schema IR (the multi-format pivot).
//
// This file pins the IR's structural surface: every type compiles,
// constructors do what they say, equality is value-based, the cyclic
// graph (AnyType → List → Box<AnyType>) terminates, and Type-by-name
// resolution walks correctly through nested references.
//
// pSSZ wire-format round-trip (the real point of the IR being
// PSIO_REFLECT'd) lands in a follow-up commit once the Box<T> codec
// adapter is in place.

#include <psio/schema_ir.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <variant>

using psio::AnyType;
using psio::Attribute;
using psio::Func;
using psio::Interface;
using psio::Member;
using psio::Package;
using psio::Resource;
using psio::Schema;
using psio::Use;
using psio::UseItem;
using psio::UseRef;
using psio::World;

using psio::schema_types::Array;
using psio::schema_types::BoundedList;
using psio::schema_types::Box;
using psio::schema_types::Custom;
using psio::schema_types::Float;
using psio::schema_types::FracPack;
using psio::schema_types::FunctionType;
using psio::schema_types::Int;
using psio::schema_types::List;
using psio::schema_types::Object;
using psio::schema_types::Option;
using psio::schema_types::Struct;
using psio::schema_types::Tuple;
using psio::schema_types::Type;
using psio::schema_types::Variant;

// ─── Box<T> ──────────────────────────────────────────────────────────

TEST_CASE("Box<T>: deep copy + value semantics", "[schema_ir][box]")
{
   Box<int> a{42};
   CHECK(*a == 42);

   // Copy: independent storage.
   Box<int> b{a};
   CHECK(*b == 42);
   *b = 7;
   CHECK(*a == 42);
   CHECK(*b == 7);

   // Move: source emptied of ownership.
   Box<int> c{std::move(b)};
   CHECK(*c == 7);

   // Equality reads through.
   Box<int> d{7};
   CHECK(c == d);
   CHECK_FALSE(c == a);
}

// ─── Primitives ──────────────────────────────────────────────────────

TEST_CASE("Int / Float primitives", "[schema_ir]")
{
   Int i32{32, true};
   Int u32{32, false};
   CHECK(i32.bits == 32);
   CHECK(i32.isSigned);
   CHECK(u32 != i32);

   Float f32{8, 23};
   Float f64{11, 52};
   CHECK(f32.exp == 8);
   CHECK(f64 != f32);
}

// ─── AnyType — variant constructors ──────────────────────────────────

TEST_CASE("AnyType: every alternative constructs cleanly",
          "[schema_ir][anytype]")
{
   AnyType a_int{Int{32, true}};
   AnyType a_float{Float{8, 23}};
   AnyType a_struct{Struct{}};
   AnyType a_object{Object{}};
   AnyType a_list{List{Box<AnyType>{Int{8, false}}}};
   AnyType a_option{Option{Box<AnyType>{Int{32, true}}}};
   AnyType a_array{Array{Box<AnyType>{Int{8, false}}, 16}};
   AnyType a_bounded{BoundedList{Box<AnyType>{Int{8, false}}, 255}};
   AnyType a_tuple{Tuple{}};
   AnyType a_variant{Variant{}};
   AnyType a_fracpack{FracPack{Box<AnyType>{Int{8, false}}}};
   AnyType a_custom{Custom{Box<AnyType>{Int{32, false}}, "name-id"}};
   AnyType a_resource{Resource{"cursor", {}, {}}};
   AnyType a_type{Type{"my-record"}};

   CHECK(std::holds_alternative<Int>(a_int.value));
   CHECK(std::holds_alternative<Float>(a_float.value));
   CHECK(std::holds_alternative<List>(a_list.value));
   CHECK(std::holds_alternative<Option>(a_option.value));
   CHECK(std::holds_alternative<Array>(a_array.value));
   CHECK(std::holds_alternative<BoundedList>(a_bounded.value));
   CHECK(std::holds_alternative<Custom>(a_custom.value));
   CHECK(std::holds_alternative<Resource>(a_resource.value));
   CHECK(std::holds_alternative<Type>(a_type.value));
}

TEST_CASE("AnyType: string/const-char* shortcuts produce Type",
          "[schema_ir][anytype]")
{
   AnyType by_string{std::string{"named-type"}};
   AnyType by_cstr{"named-type"};
   AnyType by_explicit{Type{"named-type"}};

   CHECK(std::holds_alternative<Type>(by_string.value));
   CHECK(std::holds_alternative<Type>(by_cstr.value));
   CHECK(by_string == by_cstr);
   CHECK(by_string == by_explicit);
}

TEST_CASE("AnyType: deep equality across the variant",
          "[schema_ir][anytype]")
{
   AnyType a{List{Box<AnyType>{Int{32, true}}}};
   AnyType b{List{Box<AnyType>{Int{32, true}}}};
   AnyType c{List{Box<AnyType>{Int{32, false}}}};

   CHECK(a == b);
   CHECK_FALSE(a == c);
}

// ─── Schema container + Type resolution ──────────────────────────────

TEST_CASE("Schema: insert/get round-trip", "[schema_ir][schema]")
{
   Schema s;
   s.insert("u8",  AnyType{Int{8, false}});
   s.insert("u32", AnyType{Int{32, false}});

   const auto* u8 = s.get("u8");
   REQUIRE(u8 != nullptr);
   CHECK(std::get<Int>(u8->value).bits == 8);

   CHECK(s.get("missing") == nullptr);
}

TEST_CASE("AnyType::resolve walks Type-by-name references",
          "[schema_ir][resolve]")
{
   Schema s;
   // chain: alias -> alias2 -> Int{32, false}
   s.insert("alias",  AnyType{Type{"alias2"}});
   s.insert("alias2", AnyType{Type{"u32"}});
   s.insert("u32",    AnyType{Int{32, false}});

   AnyType ref{Type{"alias"}};
   const auto* resolved = ref.resolve(s);
   REQUIRE(resolved != nullptr);
   REQUIRE(std::holds_alternative<Int>(resolved->value));
   CHECK(std::get<Int>(resolved->value).bits == 32);

   // Unknown name → nullptr.
   AnyType bad{Type{"nope"}};
   CHECK(bad.resolve(s) == nullptr);

   // Non-Type values resolve to themselves.
   AnyType direct{Int{8, false}};
   const auto* self = direct.resolve(s);
   CHECK(self == &direct);
}

// ─── Envelope (Package, Use, Interface, World) ───────────────────────

TEST_CASE("Envelope types: Package / Use / Interface / World",
          "[schema_ir][envelope]")
{
   Schema s;
   s.package = Package{
      .name       = "wasi-clocks",
      .version    = "0.2.3",
      .attributes = {{"since", std::string{"0.2.0"}}}};

   s.uses.push_back(Use{
      .package        = "wasi-io",
      .interface_name = "poll",
      .version        = "0.2.3",
      .items          = {UseItem{.name = "pollable", .alias = std::nullopt}}});

   s.interfaces.push_back(Interface{
      .name       = "wall-clock",
      .type_names = {"datetime"},
      .funcs      = {Func{.name = "now"}, Func{.name = "resolution"}},
      .attributes = {}});

   s.worlds.push_back(World{
      .name       = "imports",
      .imports    = {UseRef{"wasi-io", "poll"}},
      .exports    = {"wall-clock"},
      .attributes = {}});

   REQUIRE(s.interfaces.size() == 1);
   CHECK(s.interfaces[0].funcs.size() == 2);
   CHECK(s.interfaces[0].funcs[0].name == "now");
   CHECK(s.uses[0].items[0].name == "pollable");
   CHECK(s.worlds[0].imports[0].package == "wasi-io");

   // Whole-Schema deep equality.
   Schema t = s;
   CHECK(t == s);
   t.interfaces[0].funcs[0].name = "different";
   CHECK(t != s);
}

// ─── Resource methods (opaque handle with reflected ops) ─────────────

TEST_CASE("Resource carries methods, no data members",
          "[schema_ir][resource]")
{
   Resource r{
      .name    = "pollable",
      .methods = {Func{.name = "ready", .result = AnyType{Int{1, false}}},
                  Func{.name = "block"}},
      .attributes = {}};

   CHECK(r.methods.size() == 2);
   CHECK(r.methods[0].name == "ready");
   REQUIRE(r.methods[0].result.has_value());
}

// ─── Reflection plumbing — every IR struct is reachable ──────────────

TEST_CASE("Every IR struct is PSIO_REFLECT'd", "[schema_ir][reflect]")
{
   STATIC_REQUIRE(psio::Reflected<Attribute>);
   STATIC_REQUIRE(psio::Reflected<Object>);
   STATIC_REQUIRE(psio::Reflected<Struct>);
   STATIC_REQUIRE(psio::Reflected<Resource>);
   STATIC_REQUIRE(psio::Reflected<Array>);
   STATIC_REQUIRE(psio::Reflected<List>);
   STATIC_REQUIRE(psio::Reflected<BoundedList>);
   STATIC_REQUIRE(psio::Reflected<Option>);
   STATIC_REQUIRE(psio::Reflected<Variant>);
   STATIC_REQUIRE(psio::Reflected<Tuple>);
   STATIC_REQUIRE(psio::Reflected<Custom>);
   STATIC_REQUIRE(psio::Reflected<FracPack>);
   STATIC_REQUIRE(psio::Reflected<Int>);
   STATIC_REQUIRE(psio::Reflected<Float>);
   STATIC_REQUIRE(psio::Reflected<Type>);
   STATIC_REQUIRE(psio::Reflected<AnyType>);
   STATIC_REQUIRE(psio::Reflected<FunctionType>);
   STATIC_REQUIRE(psio::Reflected<Member>);
   STATIC_REQUIRE(psio::Reflected<Func>);
   STATIC_REQUIRE(psio::Reflected<Package>);
   STATIC_REQUIRE(psio::Reflected<UseItem>);
   STATIC_REQUIRE(psio::Reflected<Use>);
   STATIC_REQUIRE(psio::Reflected<UseRef>);
   STATIC_REQUIRE(psio::Reflected<Interface>);
   STATIC_REQUIRE(psio::Reflected<World>);
   STATIC_REQUIRE(psio::Reflected<Schema>);
}
