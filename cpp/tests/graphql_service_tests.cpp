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

// ── Flattened members ───────────────────────────────────────────────────────
namespace gap_flatten
{
   using gap_empty::Nothing;
   struct Inner
   {
      std::int32_t a      = 1;
      std::int32_t secret = 2;
      std::int32_t twice() const { return 2 * a; }
      std::string  echo(std::string s) const { return s + std::to_string(a); }
      PSIO_REFLECT(Inner, a, attr(secret, psio::graphql::hidden), method(twice), method(echo, s))
   };
   struct Deep
   {
      std::int32_t d = 4;
      Inner        inner;
      PSIO_REFLECT(Deep, d, attr(inner, psio::graphql::flatten))
   };
   struct Outer
   {
      std::int32_t b = 3;
      Deep         deep;
      Nothing      none;
      std::int32_t z = 5;
      PSIO_REFLECT(Outer, b, attr(deep, psio::graphql::flatten), attr(none, psio::graphql::flatten), z)
   };
   struct Query
   {
      Outer outer() const { return {}; }
      PSIO_REFLECT(Query, method(outer))
   };
   using api = gql::schema<Query>;
   inline std::string run(std::string_view text, std::string_view variables = {})
   {
      return run_on<api>(Query{}, text, variables);
   }
}  // namespace gap_flatten

TEST_CASE("graphql: flattened members", "[graphql][service]")
{
   // The fields of Deep and, through it, of Inner stand where `deep` is;
   // a flattened record without members adds nothing; hidden stays hidden.
   CHECK(gap_flatten::api::sdl().find("type Outer {\n  b: Int!\n  d: Int!\n  a: Int!\n  twice: Int!\n  echo(s: String!): String!\n  z: Int!\n}\n") !=
         std::string::npos);
   CHECK(gap_flatten::run(R"({ outer { z echo(s: "n") twice a d b e2: echo(s: "m") __typename } })") ==
         R"({"data":{"outer":{"z":5,"echo":"n1","twice":2,"a":1,"d":4,"b":3,"e2":"m1","__typename":"Outer"}}})");
   CHECK(gap_flatten::run("{ outer { deep { d } } }").find(R"(Cannot query field \"deep\" on type \"Outer\".)") != std::string::npos);
   CHECK(gap_flatten::run("{ outer { secret } }").find(R"(Cannot query field \"secret\" on type \"Outer\".)") != std::string::npos);
   // The member's own type is unchanged.
   CHECK(gap_flatten::api::sdl().find("type Deep") == std::string::npos);
}

// ── Argument groups ─────────────────────────────────────────────────────────
namespace gap_arguments
{
   struct Point
   {
      std::int32_t x = 0, y = 0;
      PSIO_REFLECT(Point, x, y)
   };
   struct Paging
   {
      std::optional<std::int32_t> first;
      std::optional<std::string>  after;
      std::int32_t                internal = 9;
      PSIO_REFLECT(Paging,
                   first,
                   attr(after, psio::graphql::name_as{"cursor"} | psio::graphql::doc{"Where to start."}),
                   attr(internal, psio::graphql::hidden))
   };
   PSIO_TYPE_ATTRS(Paging, psio::graphql::arguments)
   struct Filter
   {
      std::string               prefix;
      std::vector<std::int32_t> ids;
      std::optional<Point>      at;
      Paging                    paging;  // a group in a group
      PSIO_REFLECT(Filter, prefix, ids, at, paging)
   };
   PSIO_TYPE_ATTRS(Filter, psio::graphql::arguments)

   // ── A result completed by its own arguments ────────────────────────────
   struct Later
   {
      std::int32_t base = 0;
   };
   struct LaterArgs
   {
      std::int32_t add = 0;
      PSIO_REFLECT(LaterArgs, add)
   };
   PSIO_TYPE_ATTRS(LaterArgs, psio::graphql::arguments)
   struct Query
   {
      std::string  find(std::int32_t limit, Filter filter, const gql::context&, bool flag) const
      {
         std::string out = std::to_string(limit) + "|" + filter.prefix + "|";
         for (const std::int32_t id : filter.ids)
            out += std::to_string(id) + ",";
         out += "|" + (filter.at ? std::to_string(filter.at->x) + ":" + std::to_string(filter.at->y) : "-");
         out += "|" + (filter.paging.first ? std::to_string(*filter.paging.first) : "-");
         out += "|" + filter.paging.after.value_or("-") + "|" + std::to_string(filter.paging.internal);
         return out + (flag ? "|t" : "|f");
      }
      Later later(std::int32_t base) const { return {base}; }
      PSIO_REFLECT(Query, method(find, limit, filter, ctx, flag), method(later, base))
   };
   using api = gql::schema<Query>;
   inline std::string run(std::string_view text, std::string_view variables = {})
   {
      return run_on<api>(Query{}, text, variables);
   }
}  // namespace gap_arguments
template <>
struct psio::graphql::resolver<gap_arguments::Later>
{
   using arguments = gap_arguments::LaterArgs;
   static std::int32_t resolve(gap_arguments::Later&& later, const arguments& args, psio::graphql::context&)
   {
      return later.base + args.add;
   }
};

TEST_CASE("graphql: argument groups", "[graphql][service]")
{
   CHECK(gap_arguments::api::sdl().find("  find(limit: Int!, prefix: String!, ids: [Int!]!, at: PointInput, first: Int, cursor: String, flag: Boolean!): String!\n") !=
         std::string::npos);
   CHECK(gap_arguments::run(R"({ find(flag: true, cursor: "c", ids: [3, 4], prefix: "p", limit: 2, at: {x: 1, y: 2}) })") ==
         R"({"data":{"find":"2|p|3,4,|1:2|-|c|9|t"}})");
   CHECK(gap_arguments::run(R"(query ($p: String!, $ids: [Int!]!, $n: Int, $at: PointInput) { find(limit: 1, prefix: $p, ids: $ids, first: $n, at: $at, flag: false) })",
             R"({"p": "v", "ids": [8], "n": 5, "at": {"x": 6, "y": 7}})") == R"({"data":{"find":"1|v|8,|6:7|5|-|9|f"}})");
   // A member that is not optional is a required argument; a hidden one is
   // not an argument; the group's own name is not one either.
   CHECK(gap_arguments::run(R"({ find(limit: 1, ids: [], flag: true) })").find(R"(argument \"prefix\" of type \"String!\" is required)") !=
         std::string::npos);
   CHECK(gap_arguments::run(R"({ find(limit: 1, prefix: "", ids: [], flag: true, internal: 3) })").find(R"(Unknown argument \"internal\")") !=
         std::string::npos);
   CHECK(gap_arguments::run(R"({ find(limit: 1, prefix: "", ids: [], flag: true, filter: 3) })").find(R"(Unknown argument \"filter\")") !=
         std::string::npos);
   CHECK(gap_arguments::run(R"({ find(limit: 1, prefix: "", ids: [], flag: true, after: "x") })").find(R"(Unknown argument \"after\")") !=
         std::string::npos);
   // The description of a member is the argument's.
   CHECK(gap_arguments::run(R"({ __type(name: "Query") { fields { name args { name description } } } })")
            .find(R"({"name":"cursor","description":"Where to start."})") != std::string::npos);

   // A result with arguments of its own: they follow the method's.
   CHECK(gap_arguments::api::sdl().find("  later(base: Int!, add: Int!): Int!\n") != std::string::npos);
   CHECK(gap_arguments::run("{ later(add: 2, base: 40) }") == R"({"data":{"later":42}})");
   CHECK(gap_arguments::run("{ later(base: 40) }").find(R"(argument \"add\" of type \"Int!\" is required)") != std::string::npos);
}
