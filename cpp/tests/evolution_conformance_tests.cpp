// Consumes conformance/evolution-vectors.json (conformance/evolution-vectors.md):
// pairs of types and of interfaces of two Schema IRs with the verdicts of
// psio::pssz_ir::evolves_from / equivalent. The file is what the other
// languages run their own run-time checkers against.

#include "evolution_conformance_types.hpp"

#include <catch.hpp>

#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace evo_conf;
namespace ir = psio::pssz_ir;

namespace
{
   const Fixture& fixture()
   {
      static const Fixture fx = [] {
         std::ifstream     in(PSIO_EVOLUTION_VECTORS, std::ios::binary);
         std::stringstream buf;
         buf << in.rdbuf();
         const std::string text = buf.str();
         Fixture           out;
         auto st = psio::decode_json_checked(psio::json{}, std::span<const char>{text}, out);
         if (!st.ok())
            FAIL("cannot read " PSIO_EVOLUTION_VECTORS ": " << st.error().what);
         return out;
      }();
      return fx;
   }
   const std::map<std::string, psio::Schema>& schemas()
   {
      static const std::map<std::string, psio::Schema> all = [] {
         std::map<std::string, psio::Schema> out;
         for (const auto& row : fixture().schemas)
         {
            std::vector<char> bytes(row.ir_hex.size() / 2);
            REQUIRE(psio::from_hex(row.ir_hex, bytes));
            REQUIRE(psio::validate_strict<psio::Schema>(psio::pssz{}, std::span<const char>{bytes})
                       .ok());
            auto schema = psio::decode<psio::Schema>(psio::pssz{}, std::span<const char>{bytes});
            REQUIRE(ir::validate_schema(schema).ok());
            REQUIRE(out.emplace(row.id, std::move(schema)).second);
         }
         return out;
      }();
      return all;
   }
   template <typename A, typename B>
   void check(const psio::Schema& sa, const A& a, const psio::Schema& sb, const B& b,
              const Verdicts& evolves, const Verdicts& equivalent)
   {
      CHECK(verdict_of(ir::evolves_from(sa, a, sb, b, ir::compare::wire)) == evolves.wire);
      CHECK(verdict_of(ir::evolves_from(sa, a, sb, b, ir::compare::contract)) == evolves.contract);
      CHECK(verdict_of(ir::evolves_from(sa, a, sb, b)) == evolves.contract);
      CHECK(verdict_of(ir::equivalent(sa, a, sb, b, ir::compare::wire)) == equivalent.wire);
      CHECK(verdict_of(ir::equivalent(sa, a, sb, b, ir::compare::contract)) ==
            equivalent.contract);
      // A failed comparison says why, in words, and its status carries both.
      const auto result = ir::evolves_from(sa, a, sb, b);
      CHECK(result.ok == result.reason.empty());
      CHECK(result.ok == result.status().ok());
      if (!result.ok)
         CHECK(std::string{result.status().error().what} == result.path + ": " + result.reason);
   }
   const psio::Interface& interface_named(const psio::Schema& s, const std::string& name)
   {
      for (const auto& in : s.interfaces)
         if (in.name == name)
            return in;
      FAIL("no interface " << name);
      return s.interfaces.front();
   }
}  // namespace

TEST_CASE("evolution conformance: every type pair", "[schema][evolution][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.version == 1);
   REQUIRE(fx.types.size() >= 90);
   std::set<std::string> names, codes;
   for (const auto& row : fx.types)
   {
      INFO(row.name);
      CHECK(names.insert(row.name).second);
      const auto& older = schemas().at(row.old_schema);
      const auto& newer = schemas().at(row.new_schema);
      // A type by its name, and as the node itself.
      check(older, psio::AnyType{row.old_type}, newer, psio::AnyType{row.new_type}, row.evolves,
            row.equivalent);
      REQUIRE(older.get(row.old_type));
      REQUIRE(newer.get(row.new_type));
      check(older, *older.get(row.old_type), newer, *newer.get(row.new_type), row.evolves,
            row.equivalent);
      for (const auto* v : {&row.evolves.wire, &row.evolves.contract, &row.equivalent.wire,
                            &row.equivalent.contract})
         if (!v->ok)
            codes.insert(v->code);
   }
   // The fixture reaches every rule a type pair can break.
   for (const char* code :
        {"type-changed", "boxed-changed", "custom-id-changed", "enum-changed", "integer-changed",
         "array-length-changed", "utf8-changed", "optional-form-changed", "bound-tightened",
         "bound-changed", "width-changed", "key-type-changed", "alternatives-removed",
         "alternative-count-changed", "dwnc-changed", "fields-removed", "field-count-changed",
         "dwnc-gained-fields", "field-renamed", "appended-field-not-optional", "max-dynamic-data",
         "max-fields"})
   {
      INFO(code);
      CHECK(codes.count(code) == 1);
   }
}

TEST_CASE("evolution conformance: every interface pair", "[schema][evolution][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.interfaces.size() >= 12);
   for (const auto& row : fx.interfaces)
   {
      INFO(row.name);
      const auto& older = schemas().at(row.old_schema);
      const auto& newer = schemas().at(row.new_schema);
      const auto& ia    = interface_named(older, row.old_interface);
      const auto& ib    = interface_named(newer, row.new_interface);
      check(older, ia, newer, ib, row.evolves, row.equivalent);
      // Each schema has the one interface: the whole-schema comparison is
      // the interface's, under the interface's name.
      auto whole = [&](const Verdict& v) {
         Verdict out = v;
         if (!out.ok)
            out.path = row.old_interface + "/" + out.path;
         return out;
      };
      CHECK(verdict_of(ir::evolves_from(older, newer, ir::compare::wire)) ==
            whole(row.evolves.wire));
      CHECK(verdict_of(ir::evolves_from(older, newer)) == whole(row.evolves.contract));
      CHECK(verdict_of(ir::equivalent(older, newer, ir::compare::wire)) ==
            whole(row.equivalent.wire));
      CHECK(verdict_of(ir::equivalent(older, newer)) == whole(row.equivalent.contract));
   }
}
