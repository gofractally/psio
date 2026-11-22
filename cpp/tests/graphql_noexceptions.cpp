// psio::graphql with exceptions and RTTI disabled, as a WebAssembly guest
// builds it: every failure — syntax, validation, variables, limits, field
// errors — comes back as an error in the response. Nothing here may abort.

#include "graphql_fixture.hpp"

#include <cstdio>
#include <random>
#include <string>
#include <string_view>

namespace
{
   int failures = 0;

   bool json_valid(std::string_view text)
   {
      psio::json_text_cursor c{std::span<const char>{text.data(), text.size()}, true};
      return c.skip() && c.finish();
   }

   std::string run(const gqlt::Query& q, std::string_view text, std::string_view variables = {},
                   const psio::graphql::limits& lim = {})
   {
      std::string    out;
      gqlt::Mutation m;
      gqlt::api::run(q, m, text, out, psio::graphql::request{.variables = variables}, lim);
      if (!json_valid(out))
      {
         std::printf("FAIL: response is not JSON for %.*s\n", static_cast<int>(text.size()), text.data());
         ++failures;
      }
      return out;
   }

   void expect(const std::string& got, std::string_view want, const char* what)
   {
      if (got != want)
      {
         std::printf("FAIL %s\n   got  %s\n   want %.*s\n", what, got.c_str(), static_cast<int>(want.size()),
                     want.data());
         ++failures;
      }
   }
   void expect_has(const std::string& got, std::string_view part, const char* what)
   {
      if (got.find(part) == std::string::npos)
      {
         std::printf("FAIL %s\n   got  %s\n   want ...%.*s...\n", what, got.c_str(), static_cast<int>(part.size()),
                     part.data());
         ++failures;
      }
   }
}  // namespace

int main()
{
   const gqlt::Query q = gqlt::make_query(6);

   // Results, by value and through views.
   expect(run(q, "{ item(id: 1001) { id name owner status blob created note } }"),
          R"({"data":{"item":{"id":1001,"name":"item \"1\"\n","owner":"acct-1","status":"frozen","blob":"01ABFF","created":"2023-11-14T22:13:21Z","note":"note 1"}}})",
          "value");
   expect(run(q, "{ r: itemsView { id name tags { label } detail { __typename } rev title } }"),
          run(q, "{ r: items { id name tags { label } detail { __typename } rev title } }"), "view == value");
   expect(run(q, "{ oldItem { id rev } }"), R"({"data":{"oldItem":{"id":77,"rev":null}}})", "evolution");
   expect(run(q, "query ($f: FilterInput!, $n: Int = 2) { describe(filter: $f) squares(first: $n) }",
              R"({"f": {"names": ["a"], "status": "closed"}})"),
          R"({"data":{"describe":"status=closed min=- names=a, range=-","squares":[0,1]}})", "variables");
   expect_has(run(q, "{ accounts(first: 2) { edges { cursor node { id } } pageInfo { hasNextPage } } }"),
              R"("pageInfo":{"hasNextPage":true})", "connection");
   expect_has(run(q, "{ __schema { types { name } } __type(name: \"Item\") { fields { name } } }"),
              R"({"name":"__Type"})", "introspection");
   expect_has(gqlt::api::sdl(), "union Tag_or_Extra = Tag | Extra\n", "sdl");

   // Failures, each a returned error.
   expect_has(run(q, "{ items { id "), "Syntax Error: Expected Name, found <EOF>.", "syntax");
   expect_has(run(q, "{ nope }"), R"(Cannot query field \"nope\" on type \"Query\".)", "validation");
   expect_has(run(q, "{ add(a: 1, b: \"x\") }"), R"(Expected value of type \"Int\", found \"x\".)", "argument");
   expect_has(run(q, "query ($a: Int!) { add(a: $a, b: 1) }", R"({"a": "x"})"),
              R"(Variable \"$a\" got invalid value)", "variable");
   expect_has(run(q, "query ($a: Int!) { add(a: $a, b: 1) }", "{\"a\": "), "Variables are not a valid JSON object.",
              "variables json");
   expect(run(q, "{ risky(ok: false) nested { bad } strict { ok } }"),
          R"({"data":{"risky":null,"nested":null,"strict":{"ok":2}},"errors":[{"message":"risky failed","locations":[{"line":1,"column":3}],"path":["risky"]},{"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":1,"column":29}],"path":["nested","bad"]}]})",
          "field errors");
   expect_has(run(q, "{ badText }"), R"({"data":null,"errors":[)", "null propagation");
   expect_has(run(q, "{ badEnum }"), "has no name for this value", "enum");
   expect_has(run(q, "{ accounts(after: \"zz\") { edges { cursor } } }"), "is not a cursor", "cursor");
   expect_has(run(q, "mutation { failing bump(by: 1) }"), R"({"data":{"failing":null,"bump":2})", "mutation");

   // Limits.
   psio::graphql::limits lim;
   lim.max_depth = 2;
   expect_has(run(q, "{ items { tags { label } } }", {}, lim), "nested deeper than the limit allows", "depth");
   lim            = {};
   lim.max_fields = 4;
   expect_has(run(q, "{ a: nothing b: nothing c: nothing d: nothing e: nothing }", {}, lim),
              "selects more fields than the limit allows", "fields");
   lim                = {};
   lim.max_complexity = 10;
   expect_has(run(q, "{ items { id name } }", {}, lim), "Query complexity exceeds the limit of 10.", "complexity");
   lim                    = {};
   lim.max_response_bytes = 32;
   expect_has(run(q, "{ items { id name } }", {}, lim), "Response is larger than the limit allows.", "response");
   expect_has(run(q, std::string(100'000, '{')), "Document is larger than the limit allows.", "size");

   // Mutated text: always a JSON response.
   const std::string seeds[] = {
      "{ items { id name owner tags { label } detail { ... on Tag { label } ... on Extra { codes } } } }",
      "query Q($a: Int! = 4, $s: Boolean = true) { add(a: $a, b: 2) @skip(if: $s) ...F } fragment F on Query { node { id } }",
      "{ accounts(first: 2) { edges { cursor node { id title } } } __type(name: \"Item\") { name } }",
   };
   std::mt19937_64 rng{20261122};
   for (int round = 0; round < 20000; ++round)
   {
      std::string text = seeds[rng() % 3];
      for (int e = static_cast<int>(rng() % 4); e > 0 && !text.empty(); --e)
      {
         const std::size_t at = rng() % text.size();
         switch (rng() % 4)
         {
            case 0: text.erase(at, 1 + rng() % 6); break;
            case 1: text.insert(at, text.substr(rng() % text.size(), rng() % 24)); break;
            case 2: text[at] = static_cast<char>(rng()); break;
            default: text.resize(at); break;
         }
      }
      (void)run(q, text, rng() % 3 ? "" : R"({"a": 2, "s": false})");
   }

   if (failures)
      return 1;
   std::puts("graphql noexceptions: ok");
   return 0;
}
