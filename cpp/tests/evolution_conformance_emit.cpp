// Emits conformance/evolution-vectors.json: pairs of types (and of
// interfaces) of two Schema IRs with the verdicts of the run-time schema
// comparison, psio::pssz_ir::evolves_from / equivalent
// (psio/pssz_ir_compare.hpp; docs/pssz-spec.md 7.5 / 7.6). The file is
// described in conformance/evolution-vectors.md.
//
//   psio_evolution_conformance_emit <out.json>          write the fixture
//   psio_evolution_conformance_emit --check <ref.json>  fail unless <ref.json>
//                                                       is what this build
//                                                       generates
//
// Every pair of C++ types is also given to the compile-time
// psio::pssz_evolves_from<Old, New>: the emitter refuses to write a row whose
// `wire` verdict differs from it.
#include "evolution_conformance_types.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace
{
   using namespace evo_conf;
   namespace ir = psio::pssz_ir;

   [[noreturn]] void die(const std::string& what)
   {
      std::fprintf(stderr, "evolution conformance emit: %s\n", what.c_str());
      std::exit(1);
   }

   template <typename A, typename B>
   Verdicts both(const psio::Schema& sa, const A& a, const psio::Schema& sb, const B& b,
                 bool evolves)
   {
      auto run = [&](ir::compare what) {
         return verdict_of(evolves ? ir::evolves_from(sa, a, sb, b, what)
                                   : ir::equivalent(sa, a, sb, b, what));
      };
      return Verdicts{run(ir::compare::wire), run(ir::compare::contract)};
   }

   const psio::Interface& only_interface(const psio::Schema& s)
   {
      if (s.interfaces.size() != 1)
         die("an interface case has one interface per schema");
      return s.interfaces[0];
   }

   template <typename Row>
   void rows(std::ostringstream& out, const char* key, const std::vector<Row>& all, bool last)
   {
      out << "\"" << key << "\":[\n";
      for (std::size_t i = 0; i < all.size(); ++i)
         out << psio::encode(psio::json{}, all[i]) << (i + 1 < all.size() ? ",\n" : "\n");
      out << (last ? "]\n" : "],\n");
   }

   std::string render()
   {
      Fixture fx;
      fx.version = 1;
      auto schema_id = [&](const std::string& id, const psio::Schema& schema) {
         if (auto st = ir::validate_schema(schema); !st.ok())
            die(id + ": " + std::string{st.error().what});
         fx.schemas.push_back(SchemaRow{id, hex(schema)});
         return id;
      };
      for (const auto& p : pairs())
      {
         const auto older = schema_id(p.name + "/old", p.older.schema);
         const auto newer = schema_id(p.name + "/new", p.newer.schema);
         const psio::AnyType ta{p.older.root}, tb{p.newer.root};
         TypeCase forward{p.name,
                          older,
                          p.older.root,
                          newer,
                          p.newer.root,
                          both(p.older.schema, ta, p.newer.schema, tb, true),
                          both(p.older.schema, ta, p.newer.schema, tb, false)};
         TypeCase reverse{p.name + "~reverse",
                          newer,
                          p.newer.root,
                          older,
                          p.older.root,
                          both(p.newer.schema, tb, p.older.schema, ta, true),
                          both(p.newer.schema, tb, p.older.schema, ta, false)};
         if (p.compile_time && *p.compile_time != forward.evolves.wire.ok)
            die(p.name + ": the run-time verdict differs from psio::pssz_evolves_from");
         if (p.compile_time_reverse && *p.compile_time_reverse != reverse.evolves.wire.ok)
            die(p.name + "~reverse: the run-time verdict differs from psio::pssz_evolves_from");
         // `equivalent` is symmetric, and implies `evolves_from` both ways.
         if (forward.equivalent.wire.ok != reverse.equivalent.wire.ok ||
             forward.equivalent.contract.ok != reverse.equivalent.contract.ok)
            die(p.name + ": equivalent is not symmetric");
         for (auto [eq, a, b] : {std::tuple{&forward.equivalent.wire, &forward.evolves.wire,
                                            &reverse.evolves.wire},
                                 std::tuple{&forward.equivalent.contract, &forward.evolves.contract,
                                            &reverse.evolves.contract}})
            if (eq->ok && !(a->ok && b->ok))
               die(p.name + ": equivalent types that do not evolve from each other");
         // The contract includes the wire.
         for (auto v : {&forward.evolves, &forward.equivalent, &reverse.evolves,
                        &reverse.equivalent})
            if (v->contract.ok && !v->wire.ok)
               die(p.name + ": compatible by contract but not on the wire");
         fx.types.push_back(std::move(forward));
         fx.types.push_back(std::move(reverse));
      }
      for (const auto& p : interface_pairs())
      {
         const auto  older = schema_id(p.name + "/old", p.older);
         const auto  newer = schema_id(p.name + "/new", p.newer);
         const auto &ia = only_interface(p.older), &ib = only_interface(p.newer);
         fx.interfaces.push_back(InterfaceCase{p.name, older, ia.name, newer, ib.name,
                                               both(p.older, ia, p.newer, ib, true),
                                               both(p.older, ia, p.newer, ib, false)});
         fx.interfaces.push_back(InterfaceCase{p.name + "~reverse", newer, ib.name, older, ia.name,
                                               both(p.newer, ib, p.older, ia, true),
                                               both(p.newer, ib, p.older, ia, false)});
      }
      std::ostringstream out;
      out << "{\"version\":1,\n";
      rows(out, "schemas", fx.schemas, false);
      rows(out, "types", fx.types, false);
      rows(out, "interfaces", fx.interfaces, true);
      out << "}\n";
      return out.str();
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   if (!check && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json>\n", argv[0]);
      return 2;
   }
   const std::string text = render();
   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "evolution conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended change of the comparison\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   return out ? 0 : 1;
}
