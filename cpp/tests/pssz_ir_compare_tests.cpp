// psio::pssz_ir::equivalent / evolves_from (psio/pssz_ir_compare.hpp): the
// run-time comparison of two types, functions, interfaces or whole schemas.
// The shared rows are conformance/evolution-vectors.json
// (evolution_conformance_tests.cpp); the pairs of the compile-time
// psio::pssz_evolves_from tests are replayed through the run-time check
// where they stand (evolves_parity.hpp).

#include "evolves_parity.hpp"

#include <catch.hpp>

#include <psio/invariant.hpp>
#include <psio/json.hpp>
#include <psio/pssz_ir_json.hpp>

#include <map>

namespace ir = psio::pssz_ir;
namespace st = psio::schema_types;

namespace cmp
{
   struct A
   {
      std::uint32_t                x = 0;
      std::optional<std::uint16_t> y;
   };
   PSIO_REFLECT(A, x, y)

   // The same bytes: a u64 under a name with an invariant.
   struct Id
   {
      std::uint64_t value = 0;
      PSIO_REFLECT(Id, transparent(), value)
   };
   inline psio::codec_status id_ok(const Id&) noexcept { return psio::codec_ok(); }
}  // namespace cmp
PSIO_INVARIANT(cmp::Id, cmp::id_ok, "Id")
namespace cmp
{

   struct Row
   {
      std::uint64_t     id = 0;
      std::string       owner;
      std::vector<char> data;
      PSIO_REFLECT(Row, id, owner, data, maxDynamicData(4000))
   };
   struct RowV2
   {
      std::uint64_t              id = 0;
      std::string                owner;
      std::vector<char>          data;
      std::optional<std::string> note;
      PSIO_REFLECT(RowV2, id, owner, data, note, maxDynamicData(4000))
   };

   namespace v1
   {
      struct Tokens
      {
         void          credit(std::uint64_t to, std::uint64_t amount);
         std::uint64_t balance(std::uint64_t of);
      };
      PSIO_REFLECT(Tokens, method(credit, to, amount, maxDynamicData(100)), method(balance, of))
      PSIO_INTERFACE(Tokens, name("tokens"))
      struct Events
      {
         void credited(std::uint64_t to, std::uint64_t amount);
      };
      PSIO_REFLECT(Events, method(credited, to, amount, maxDynamicData(100)))
      PSIO_INTERFACE(Events, name("tokens.history"))
   }
   namespace v2
   {
      struct Tokens
      {
         void          credit(std::uint64_t to, std::uint64_t amount, std::optional<std::string> memo);
         std::uint64_t balance(std::uint64_t of);
         void          burn(std::uint64_t amount);
      };
      PSIO_REFLECT(Tokens,
                   method(credit, to, amount, memo, maxDynamicData(100)),
                   method(balance, of),
                   method(burn, amount))
      PSIO_INTERFACE(Tokens, name("tokens"))
      struct Events
      {
         void credited(std::uint64_t to, std::uint64_t amount, std::optional<std::string> memo);
      };
      PSIO_REFLECT(Events, method(credited, to, amount, memo, maxDynamicData(100)))
      PSIO_INTERFACE(Events, name("tokens.history"))
   }
}  // namespace cmp

TEST_CASE("schema comparison: one type in two schemas is equivalent however it is interned",
          "[schema][compare]")
{
   // (the probe of the gap: Schema::get(...) == compares the names)
   auto              s1 = psio::SchemaBuilder{}.insert<cmp::A>("A").build();
   psio::SchemaBuilder b;
   b.insert<std::uint64_t>("u64");  // same A, another interning order
   auto s2 = std::move(b).insert<cmp::A>("A").build();
   CHECK(ir::equivalent(s1, "A", s2, "A"));
   CHECK(ir::equivalent(s1, *s1.get("A"), s2, *s2.get("A")));
   CHECK(ir::evolves_from(s1, "A", s2, "A"));
   CHECK(ir::equivalent(s1, "A", s2, "A", ir::compare::wire));
   // The result is a value that says what and where.
   const auto no = ir::equivalent(s1, "A", s2, "u64");
   CHECK_FALSE(no);
   CHECK(no.code == "type-changed");
   CHECK(no.path == "(root)");
   CHECK(no.reason == "the type differs (record / integer)");
   CHECK(std::string{no.status().error().what} == "(root): the type differs (record / integer)");
}

TEST_CASE("schema comparison: a malformed or unknown type is reported, not compared",
          "[schema][compare]")
{
   auto s = psio::SchemaBuilder{}.insert<cmp::A>("A").build();
   auto r = ir::evolves_from(s, "A", s, "Missing");
   CHECK_FALSE(r);
   CHECK(r.code == "malformed-schema");
   CHECK(r.reason == "the second schema: pssz IR: unresolved named type");
   // A schema with no version marker is not read (schema_ir.hpp).
   auto unversioned = s;
   unversioned.ir_version.reset();
   r = ir::equivalent(unversioned, "A", s, "A");
   CHECK(r.code == "malformed-schema");
   CHECK(r.reason.starts_with("the first schema: pssz IR: schema has no ir_version marker"));
}

TEST_CASE("schema comparison: contract and wire", "[schema][compare]")
{
   // A custom type is its wire type on the wire and a type of its own by
   // contract.
   CHECK(psio_test::runtime_equivalent<cmp::Id, std::uint64_t>(ir::compare::wire));
   const auto r = psio_test::runtime_equivalent<cmp::Id, std::uint64_t>();
   CHECK(r.code == "custom-id-changed");
   CHECK(psio_test::runtime_equivalent<cmp::Id, cmp::Id>());
   CHECK(psio_test::runtime_equivalent<std::vector<cmp::Id>, std::vector<cmp::Id>>());
   // std::vector<char> and std::vector<std::uint8_t> are one node.
   CHECK(psio_test::runtime_equivalent<std::vector<char>, std::vector<std::uint8_t>>());
   CHECK(psio_test::runtime_equivalent<std::vector<char>, std::vector<std::byte>>());
   // ... and neither is a list of small signed integers, or a string.
   CHECK(psio_test::runtime_equivalent<std::vector<char>, std::vector<signed char>>().code ==
         "type-changed");
   CHECK(psio_test::runtime_equivalent<std::vector<char>, std::string>().code == "type-changed");
   // A tuple and a pair; a map whatever container holds it.
   CHECK(psio_test::runtime_equivalent<std::pair<int, std::string>, std::tuple<int, std::string>>());
   CHECK(psio_test::runtime_equivalent<std::map<int, int>, std::unordered_map<int, int>>());
}

TEST_CASE("schema comparison: a table row may be upgraded, its key may not change",
          "[schema][compare]")
{
   // What a service registry does when a service publishes a new schema.
   auto installed = psio::SchemaBuilder{}.insert<cmp::Row>("Row").build();
   auto proposed  = psio::SchemaBuilder{}.insert<cmp::RowV2>("Row").build();
   CHECK(ir::evolves_from(installed, "Row", proposed, "Row"));
   CHECK_FALSE(ir::equivalent(installed, "Row", proposed, "Row"));
   const auto back = ir::evolves_from(proposed, "Row", installed, "Row");
   CHECK(back.code == "fields-removed");
   // The type of a key field, taken out of each record.
   auto key = [](const psio::Schema& s, std::size_t i) -> const psio::AnyType& {
      return *std::get<st::Object>(s.get("Row")->resolve(s)->value).members[i].type;
   };
   CHECK(ir::equivalent(installed, key(installed, 0), proposed, key(proposed, 0)));
   CHECK(ir::equivalent(installed, key(installed, 1), proposed, key(proposed, 1)));
   CHECK(ir::equivalent(installed, key(installed, 0), proposed, key(proposed, 1)).code ==
         "type-changed");
}

TEST_CASE("schema comparison: functions, interfaces and whole schemas", "[schema][compare]")
{
   auto older = psio::SchemaBuilder{}
                   .insert_interface<cmp::v1::Tokens>()
                   .insert_interface<cmp::v1::Events>()
                   .build();
   auto newer = psio::SchemaBuilder{}
                   .insert_interface<cmp::v2::Tokens>()
                   .insert_interface<cmp::v2::Events>()
                   .build();
   REQUIRE(older.interfaces.size() == 2);
   CHECK(ir::equivalent(older, older));
   CHECK(ir::evolves_from(older, newer));
   CHECK(ir::evolves_from(older, newer, ir::compare::wire));

   auto r = ir::evolves_from(newer, older);
   CHECK(r.code == "fields-removed");
   CHECK(r.path == "tokens/credit(args)");
   r = ir::equivalent(older, newer);
   CHECK(r.code == "field-count-changed");
   CHECK(r.path == "tokens/credit(args)");

   // One interface, one function.
   CHECK(ir::evolves_from(older, older.interfaces[1], newer, newer.interfaces[1]));
   r = ir::equivalent(older, older.interfaces[1], newer, newer.interfaces[1]);
   CHECK(r.path == "credited(args)");
   const auto &credit1 = older.interfaces[0].funcs[0], &credit2 = newer.interfaces[0].funcs[0];
   CHECK(ir::evolves_from(older, credit1, newer, credit2));
   CHECK(ir::equivalent(older, credit1, newer, credit2).path == "(args)");
   // The functions of an interface are matched by name, whatever their order.
   auto shuffled = newer;
   std::swap(shuffled.interfaces[0].funcs[0], shuffled.interfaces[0].funcs[2]);
   std::swap(shuffled.interfaces[0], shuffled.interfaces[1]);
   CHECK(ir::equivalent(newer, shuffled));
   CHECK(ir::evolves_from(older, shuffled));
   // A function that is gone, an interface that is gone.
   auto fewer = older;
   fewer.interfaces[0].funcs.pop_back();
   r = ir::evolves_from(older, fewer);
   CHECK(r.code == "function-removed");
   CHECK(r.path == "tokens/balance");
   CHECK(ir::evolves_from(fewer, older));
   CHECK(ir::equivalent(fewer, older).code == "function-added");
   fewer = older;
   fewer.interfaces.pop_back();
   r = ir::evolves_from(older, fewer);
   CHECK(r.code == "interface-removed");
   CHECK(r.path == "tokens.history/");
   CHECK(ir::equivalent(fewer, older).code == "interface-added");

   // The argument list of a function is a type: what the comparison uses.
   const auto args = ir::argument_list(credit2);
   std::vector<char> bytes;
   REQUIRE(ir::json_to_pssz(newer, args, psio::ijson{}, R"({"to":"7","amount":"5"})", bytes).ok());
   CHECK(bytes == psio::encode(psio::pssz{},
                               psio::reflect<cmp::v2::Tokens>::method_arg_tuple<0>{
                                  std::tuple{std::uint64_t{7}, std::uint64_t{5},
                                             std::optional<std::string>{}}}));
}

TEST_CASE("schema comparison: listed types of an interface", "[schema][compare]")
{
   st::Schema older = psio::SchemaBuilder{}.insert<cmp::Row>("Row").build();
   st::Schema newer = psio::SchemaBuilder{}.insert<cmp::RowV2>("Row").build();
   older.interfaces.push_back(st::Interface{"tables", {"Row"}, {}, {}});
   newer.interfaces.push_back(st::Interface{"tables", {"Row"}, {}, {}});
   CHECK(ir::evolves_from(older, newer));
   auto r = ir::evolves_from(newer, older);
   CHECK(r.code == "fields-removed");
   CHECK(r.path == "tables/type:Row");
   newer.interfaces[0].type_names.clear();
   r = ir::evolves_from(older, newer);
   CHECK(r.code == "type-removed");
   CHECK(ir::equivalent(newer, older).code == "type-added");
}
