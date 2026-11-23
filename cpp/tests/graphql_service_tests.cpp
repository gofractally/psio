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

// ── Class templates ─────────────────────────────────────────────────────────
namespace gap_template
{
   template <typename T>
   struct Box
   {
      T v{};
      PSIO_REFLECT(Box, v)
   };
   struct Query
   {
      Box<std::int32_t>                   box() const { return {7}; }
      Box<std::optional<std::string>>     maybe() const { return {}; }
      Box<std::vector<Box<std::int32_t>>> boxes() const { return {{{1}, {2}}}; }
      Box<std::string>                    text() const { return {"t"}; }
      std::int32_t                        unbox(Box<std::int32_t> b) const { return b.v; }
      PSIO_REFLECT(Query, method(box), method(maybe), method(boxes), method(text), method(unbox, b))
   };
   using api = gql::schema<Query>;
   inline std::string run(std::string_view text, std::string_view variables = {})
   {
      return run_on<api>(Query{}, text, variables);
   }
}  // namespace gap_template

TEST_CASE("graphql: instantiations of a class template have distinct names", "[graphql][service]")
{
   const std::string sdl = gap_template::api::sdl();
   for (const std::string_view part : {
           "  box: Box_Int!\n  maybe: Box_OptionalString!\n  boxes: Box_Box_IntList!\n  text: Box_String!\n  unbox(b: Box_IntInput!): Int!\n",
           "type Box_Int {\n  v: Int!\n}\n",
           "type Box_OptionalString {\n  v: String\n}\n",
           "type Box_Box_IntList {\n  v: [Box_Int!]!\n}\n",
           "input Box_IntInput {\n  v: Int!\n}\n",
           "type Box_String {\n  v: String!\n}\n",
        })
   {
      INFO(part);
      CHECK(sdl.find(part) != std::string::npos);
   }
   CHECK(gap_template::run("{ box { v __typename } boxes { v { v } __typename } unbox(b: {v: 4}) }") ==
         R"({"data":{"box":{"v":7,"__typename":"Box_Int"},"boxes":{"v":[{"v":1},{"v":2}],"__typename":"Box_Box_IntList"},"unbox":4}})");
   static_assert(gql::affixed_name<"Pre", gap_template::Box<std::int32_t>, "Post"> == "PreBox_IntPost");
   static_assert(gql::affixed_name<"", std::string, "Row"> == "StringRow");
}
