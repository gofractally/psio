// libraries/psio/cpp/tests/wit_types_tests.cpp
//
// Coverage for psio/wit_types.hpp — primitive index encoding and
// reflection wiring on the WIT IR data structures.

#include <psio/wit_types.hpp>

#include <catch.hpp>

#include <climits>
#include <cstdint>

TEST_CASE("wit_prim ↔ type_idx round-trip", "[wit_types]")
{
   // Each enumerator maps to a unique negative idx; round-trips back.
   for (auto p : {psio::wit_prim::bool_, psio::wit_prim::u8,
                  psio::wit_prim::s8, psio::wit_prim::u16,
                  psio::wit_prim::s16, psio::wit_prim::u32,
                  psio::wit_prim::s32, psio::wit_prim::u64,
                  psio::wit_prim::s64, psio::wit_prim::f32,
                  psio::wit_prim::f64, psio::wit_prim::char_,
                  psio::wit_prim::string_})
   {
      const auto idx = psio::wit_prim_idx(p);
      REQUIRE(psio::is_prim_idx(idx));
      REQUIRE(idx < 0);
      REQUIRE(psio::idx_to_prim(idx) == p);
   }

   // Non-negative indices are *not* primitives.
   REQUIRE_FALSE(psio::is_prim_idx(0));
   REQUIRE_FALSE(psio::is_prim_idx(7));
   REQUIRE(psio::is_prim_idx(-1));
}

TEST_CASE("WIT_NO_TYPE sentinel", "[wit_types]")
{
   STATIC_REQUIRE(psio::WIT_NO_TYPE == INT32_MIN);
}

TEST_CASE("wit_type_kind values stable", "[wit_types]")
{
   // Wire-stable enum values — reordering would break already-emitted
   // schema artefacts.  Pin them.
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::record_)   == 0);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::variant_)  == 1);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::enum_)     == 2);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::flags_)    == 3);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::list_)     == 4);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::option_)   == 5);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::result_)   == 6);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::tuple_)    == 7);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::resource_) == 8);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::own_)      == 9);
   STATIC_REQUIRE(static_cast<int>(psio::wit_type_kind::borrow_)   == 10);
}

TEST_CASE("wit_attribute reflects", "[wit_types][reflect]")
{
   using R = psio::reflect<psio::wit_attribute>;
   STATIC_REQUIRE(R::is_reflected);
   STATIC_REQUIRE(R::member_count == 3);
   STATIC_REQUIRE(R::member_name<0> == "name");
   STATIC_REQUIRE(R::member_name<1> == "arg_key");
   STATIC_REQUIRE(R::member_name<2> == "arg_value");
}

TEST_CASE("wit_named_type reflects", "[wit_types][reflect]")
{
   using R = psio::reflect<psio::wit_named_type>;
   STATIC_REQUIRE(R::member_count == 3);
   STATIC_REQUIRE(R::member_name<0> == "name");
   STATIC_REQUIRE(R::member_name<1> == "type_idx");
   STATIC_REQUIRE(R::member_name<2> == "attributes");
}

TEST_CASE("wit_type_def reflects", "[wit_types][reflect]")
{
   using R = psio::reflect<psio::wit_type_def>;
   STATIC_REQUIRE(R::member_count == 7);
}

TEST_CASE("wit_func reflects", "[wit_types][reflect]")
{
   using R = psio::reflect<psio::wit_func>;
   STATIC_REQUIRE(R::member_count == 5);
}

TEST_CASE("wit_interface reflects", "[wit_types][reflect]")
{
   using R = psio::reflect<psio::wit_interface>;
   STATIC_REQUIRE(R::member_count == 4);
}

TEST_CASE("wit_world reflects + back-compat alias", "[wit_types][reflect]")
{
   using R = psio::reflect<psio::wit_world>;
   STATIC_REQUIRE(R::is_reflected);
   STATIC_REQUIRE(R::member_count == 8);  // package, name, wit_source, types, funcs, exports, imports, attributes

   // Back-compat alias for psizam consumers.
   STATIC_REQUIRE(std::is_same_v<psio::pzam_wit_world,
                                  psio::wit_world>);
}

TEST_CASE("wit_world is constructible and round-trippable in memory",
          "[wit_types]")
{
   psio::wit_world w;
   w.package = "test:inventory@1.0.0";
   w.name    = "inventory";
   w.types.push_back({.name = "item",
                       .kind = static_cast<std::uint8_t>(
                          psio::wit_type_kind::record_),
                       .fields = {{.name = "id",
                                   .type_idx = psio::wit_prim_idx(
                                      psio::wit_prim::u64)},
                                  {.name = "qty",
                                   .type_idx = psio::wit_prim_idx(
                                      psio::wit_prim::u32)}}});
   REQUIRE(w.types.size() == 1);
   REQUIRE(w.types[0].fields.size() == 2);
   REQUIRE(w.types[0].fields[0].name == "id");
   REQUIRE(psio::is_prim_idx(w.types[0].fields[0].type_idx));
   REQUIRE(psio::idx_to_prim(w.types[0].fields[0].type_idx)
           == psio::wit_prim::u64);
}
