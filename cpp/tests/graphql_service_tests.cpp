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

// ── Key ranges over any source ─────────────────────────────────────────────
namespace gap_ranges
{
   using gap_template::Box;
   // A row_source with the required members only: ten rows keyed by one byte.
   struct Digits
   {
      int  at = 10;
      char k  = 0;
      bool on() const { return at >= 0 && at < 10; }
      bool seek_first()
      {
         at = 0;
         return true;
      }
      bool seek_last()
      {
         at = 9;
         return true;
      }
      bool seek_after(std::span<const char> key)
      {
         at = key.empty() ? 0 : static_cast<unsigned char>(key[0]) + 1;
         return on();
      }
      bool seek_before(std::span<const char> key)
      {
         at = key.empty() ? -1 : std::min<int>(static_cast<unsigned char>(key[0]), 10) - 1;
         return on();
      }
      bool next()
      {
         ++at;
         return on();
      }
      bool prev()
      {
         --at;
         return on();
      }
      std::span<const char> key()
      {
         k = static_cast<char>(at);
         return {&k, 1};
      }
      std::int32_t row() const { return at * at; }
   };
   // An ordered_index whose rows are strings and whose keys are owned.
   struct Words
   {
      using map_t    = std::map<std::string, std::string, std::less<>>;
      using iterator = map_t::const_iterator;
      const map_t* rows;
      iterator     begin() const { return rows->begin(); }
      iterator     end() const { return rows->end(); }
      iterator     lower_bound(std::span<const char> k) const { return rows->lower_bound(std::string_view{k.data(), k.size()}); }
      iterator     upper_bound(std::span<const char> k) const { return rows->upper_bound(std::string_view{k.data(), k.size()}); }
      std::string  key(iterator it) const { return it->first; }
      const std::string& row(iterator it) const { return it->second; }
   };
   template <typename K>
   std::string key_of(const K& k)
   {
      const auto bytes = psio::to_key(k);
      return std::string(bytes.data(), bytes.size());
   }

   struct Query
   {
      Words::map_t words;  // by the word
      Words::map_t pairs;  // by (number, word)

      auto  digits(std::optional<std::int32_t> from, std::optional<std::int32_t> below, gql::page_args page) const
      {
         gql::key_bounds bounds;
         if (from)
         {
            const char k = static_cast<char>(*from);
            bounds.at_least({&k, 1});
         }
         if (below)
         {
            const char k = static_cast<char>(*below);
            bounds.below({&k, 1});
         }
         return gql::paginate(Digits{}, bounds, page, 4);
      }
      auto wordsIn(gql::key_range<std::string> range, gql::page_args page) const
      {
         return gql::paginate(Words{&words}, range, page);
      }
      // The key is (number, word); the range is over its first element.
      auto pairsIn(gql::key_range<std::uint32_t> range, gql::page_args page) const
      {
         return gql::paginate(gql::transform(Words{&pairs}, [](const std::string& w) { return Box<std::string>{w}; }),
                              range, page);
      }
   };
   PSIO_REFLECT(Query,
                attr(words, psio::graphql::hidden),
                attr(pairs, psio::graphql::hidden),
                method(digits, from, below, page),
                method(wordsIn, range, page),
                method(pairsIn, range, page))
   using api = gql::schema<Query>;

   inline Query make()
   {
      Query q;
      for (const char* w : {"a", "ab", "abc", "b", "", "ba"})
         q.words[key_of(std::string{w})] = w;
      for (std::uint32_t n = 0; n < 4; ++n)
         for (const char* w : {"x", "y"})
            q.pairs[key_of(std::tuple<std::uint32_t, std::string>{n, w})] = std::to_string(n) + w;
      return q;
   }
   inline std::string run(std::string_view text, std::string_view variables = {})
   {
      static const Query q = make();
      return run_on<api>(q, text, variables);
   }
}  // namespace gap_ranges

TEST_CASE("graphql: key ranges over any source", "[graphql][service][connection]")
{
   auto nodes = [](const std::string& out) {
      // The text between "edges":[ and ]: the nodes, compactly.
      std::string s;
      constexpr std::string_view key = "\"node\":";
      for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + 1))
      {
         const std::size_t from = at + key.size();
         s += out.substr(from, out.find('}', from) - from) + " ";
      }
      return s;
   };
   // key_bounds over a source that has neither seek_at_or_after nor
   // row_bytes; a page of at most 4.
   CHECK(gap_ranges::run("{ digits(from: 3, below: 9) { edges { node } pageInfo { hasNextPage hasPreviousPage startCursor endCursor } } }") ==
         R"({"data":{"digits":{"edges":[{"node":9},{"node":16},{"node":25},{"node":36}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"startCursor":"03","endCursor":"06"}}}})");
   CHECK(nodes(gap_ranges::run(R"({ digits(from: 3, below: 9, after: "06") { edges { node } } })")) == "49 64 ");
   CHECK(nodes(gap_ranges::run(R"({ digits(from: 0, last: 2) { edges { node } } })")) == "64 81 ");
   CHECK(nodes(gap_ranges::run(R"({ digits(from: 3, below: 9, last: 2, before: "05") { edges { node } } })")) == "9 16 ");
   CHECK(nodes(gap_ranges::run(R"({ digits(below: 2) { edges { node } } })")) == "0 1 ");
   CHECK(nodes(gap_ranges::run(R"({ digits(from: 9) { edges { node } } })")) == "81 ");
   CHECK(nodes(gap_ranges::run(R"({ digits(from: 5, below: 5) { edges { node } } })")).empty());
   CHECK(gap_ranges::api::sdl().find("type IntConnection {\n  edges: [IntEdge!]!\n  pageInfo: PageInfo!\n}\n") != std::string::npos);

   // String keys: a longer word is not "inside" its prefix — `gt: "a"`
   // keeps "ab" (the key encoding of a whole value is no prefix of another).
   CHECK(nodes(gap_ranges::run(R"({ wordsIn(gt: "a") { edges { node } } })")) == R"("ab" "abc" "b" "ba" )");
   CHECK(nodes(gap_ranges::run(R"({ wordsIn(le: "a") { edges { node } } })")) == R"("" "a" )");
   CHECK(nodes(gap_ranges::run(R"({ wordsIn(ge: "ab", lt: "b") { edges { node } } })")) == R"("ab" "abc" )");
   CHECK(nodes(gap_ranges::run(R"({ wordsIn(gt: "abc", le: "b", last: 5) { edges { node } } })")) == R"("b" )");

   // A range over the first element of a composite key: `gt` skips, and
   // `le` includes, every row that starts with the bound.
   CHECK(nodes(gap_ranges::run("{ pairsIn(gt: 1) { edges { node { v } } } }")) == R"({"v":"2x" {"v":"2y" {"v":"3x" {"v":"3y" )");
   CHECK(nodes(gap_ranges::run("{ pairsIn(le: 1) { edges { node { v } } } }")) == R"({"v":"0x" {"v":"0y" {"v":"1x" {"v":"1y" )");
   CHECK(nodes(gap_ranges::run("{ pairsIn(ge: 1, lt: 3, last: 3) { edges { node { v } } } }")) == R"({"v":"1y" {"v":"2x" {"v":"2y" )");
   CHECK(nodes(gap_ranges::run("{ pairsIn(ge: 2, le: 2) { edges { node { v } } } }")) == R"({"v":"2x" {"v":"2y" )");

   // A bound that has no key is a field error.
   CHECK(gap_ranges::run("{ wordsIn(ge: \"\xff\") { edges { node } } }").find("errors") != std::string::npos);
}

// The concepts, as the documentation states them.
static_assert(gql::row_source<gap_ranges::Digits>);
static_assert(!gql::ordered_index<gap_ranges::Digits>);
static_assert(gql::ordered_index<gap_ranges::Words>);
static_assert(gql::row_source<gql::index_source<gap_ranges::Words>>);
static_assert(gql::argument_group<gql::page_args> && gql::argument_group<gql::key_range<int>>);
static_assert(!gql::argument_group<gap_arguments::Point> && !gql::argument_group<int>);

// ── A service-shaped schema ────────────────────────────────────────────────
#include "graphql_service_checks.hpp"

static_assert(gql::ordered_index<gqls::Index<gqls::Block, std::uint32_t>>);

TEST_CASE("graphql: a service-shaped schema", "[graphql][service]")
{
   const std::vector<std::string> failures = gqls::service_failures();
   for (const std::string& f : failures)
      FAIL_CHECK(f);
   CHECK(failures.empty());
}
