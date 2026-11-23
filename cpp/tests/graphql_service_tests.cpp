// psio::graphql as a service uses it (docs/graphql.md: "Records without
// members", "Class templates", "Flattened members", "Argument groups",
// "Connections"): each rule on a small schema of its own, then the
// service-shaped schema of graphql_service_fixture.hpp.

#include <psio/graphql.hpp>
#include <psio/key.hpp>

#include <catch.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace gql = psio::graphql;

namespace
{
   template <typename Api, typename Root>
   std::string run_on(const Root& root, std::string_view text, std::string_view variables = {})
   {
      std::string out;
      Api::run(root, text, out, gql::request{.variables = variables});
      return out;
   }
}  // namespace

// ── Records without members ─────────────────────────────────────────────────
namespace gap_empty
{
   struct Nothing
   {
      PSIO_REFLECT(Nothing)
   };
   struct Query
   {
      Nothing nothing() const { return {}; }
      PSIO_REFLECT(Query, method(nothing))
   };
   using api = gql::schema<Query>;
   inline std::string run(std::string_view text, std::string_view variables = {})
   {
      return run_on<api>(Query{}, text, variables);
   }
}  // namespace gap_empty

TEST_CASE("graphql: a record without members has the field _", "[graphql][service]")
{
   CHECK(gap_empty::run("{ nothing { _ __typename } }") == R"({"data":{"nothing":{"_":true,"__typename":"Nothing"}}})");
   CHECK(gap_empty::api::sdl().find("type Nothing {\n  _: Boolean\n}\n") != std::string::npos);
   // ... as the query root too.
   std::string out;
   gql::schema<gap_empty::Nothing>::run(gap_empty::Nothing{}, "{ _ __typename }", out);
   CHECK(out == R"({"data":{"_":true,"__typename":"Nothing"}})");
   CHECK(gql::schema<gap_empty::Nothing>::sdl().starts_with("schema {\n  query: Nothing\n}\n\ntype Nothing {\n  _: Boolean\n}\n"));
   // A selection is still required.
   CHECK(gap_empty::run("{ nothing }").find("must have a selection of subfields") != std::string::npos);
}
