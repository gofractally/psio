#pragma once
// The checks on the service-shaped schema (graphql_service_fixture.hpp), as
// a function that returns what failed: graphql_service_tests.cpp runs it
// under the test framework and graphql_service_noexceptions.cpp with
// exceptions and RTTI disabled, as a WebAssembly guest builds the engine.

#include "graphql_service_fixture.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace gqls
{
   struct checker
   {
      Db                       db = make_db();
      std::vector<std::string> failures;

      std::string run(std::string_view text, std::string_view variables = {}, const gql::limits& lim = {})
      {
         std::string out;
         api::run(Query{&db}, text, out, gql::request{.variables = variables}, lim);
         psio::json_text_cursor c{std::span<const char>{out.data(), out.size()}, true};
         if (!(c.skip() && c.finish()))
            failures.push_back("response is not JSON for " + std::string{text});
         // The schema without introspection answers the same.
         std::string lean;
         lean_api::run(Query{&db}, text, lean, gql::request{.variables = variables}, lim);
         if (lean != out && text.find("__type") == std::string_view::npos &&
             text.find("__schema") == std::string_view::npos)
            failures.push_back("schema without introspection differs for " + std::string{text});
         return out;
      }
      void expect(const std::string& got, std::string_view want, std::string_view what)
      {
         if (got != want)
            failures.push_back(std::string{what} + "\n   got  " + got + "\n   want " + std::string{want});
      }
      void expect_has(const std::string& got, std::string_view part, std::string_view what)
      {
         if (got.find(part) == std::string::npos)
            failures.push_back(std::string{what} + "\n   got  " + got + "\n   want ..." + std::string{part} + "...");
      }
      void expect_true(bool ok, std::string_view what)
      {
         if (!ok)
            failures.push_back(std::string{what});
      }
      // The values of "blockNum":N in a response, in order.
      static std::vector<std::uint32_t> block_nums(const std::string& out)
      {
         std::vector<std::uint32_t> nums;
         constexpr std::string_view key = "\"blockNum\":";
         for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + 1))
            nums.push_back(static_cast<std::uint32_t>(std::strtoul(out.c_str() + at + key.size(), nullptr, 10)));
         return nums;
      }
      // The string value of the first `"name":"..."` in a response.
      static std::string string_of(const std::string& out, std::string_view name)
      {
         const std::string key = "\"" + std::string{name} + "\":\"";
         const std::size_t at  = out.find(key);
         if (at == std::string::npos)
            return {};
         const std::size_t end = out.find('"', at + key.size());
         return out.substr(at + key.size(), end - at - key.size());
      }

      void schema()
      {
         expect(api::sdl(), expected_sdl, "sdl");
         // The arguments of a group are arguments like any other.
         expect_has(run(R"({ __type(name: "Query") { fields { name args { name type { name kind } } } } })"),
                    R"({"name":"blocks","args":[{"name":"gt","type":{"name":"Int","kind":"SCALAR"}},{"name":"ge","type":{"name":"Int","kind":"SCALAR"}},{"name":"lt","type":{"name":"Int","kind":"SCALAR"}},{"name":"le","type":{"name":"Int","kind":"SCALAR"}},{"name":"first","type":{"name":"Int","kind":"SCALAR"}},{"name":"last","type":{"name":"Int","kind":"SCALAR"}},{"name":"before","type":{"name":"String","kind":"SCALAR"}},{"name":"after","type":{"name":"String","kind":"SCALAR"}}]})",
                    "introspection: argument group");
         // A flattened member's fields are the type's own; a marker has `_`.
         expect(run(R"({ e: __type(name: "InviteEventWithBlockContext") { kind fields { name type { kind name ofType { name } } } } m: __type(name: "TLSInfo") { fields { name type { kind name } } } })"),
                R"({"data":{"e":{"kind":"OBJECT","fields":[{"name":"block","type":{"kind":"NON_NULL","name":null,"ofType":{"name":"EventBlockInfo"}}},{"name":"inviteId","type":{"kind":"NON_NULL","name":null,"ofType":{"name":"Int"}}},{"name":"actor","type":{"kind":"NON_NULL","name":null,"ofType":{"name":"AccountNumber"}}},{"name":"event","type":{"kind":"NON_NULL","name":null,"ofType":{"name":"String"}}}]},"m":{"fields":[{"name":"_","type":{"kind":"SCALAR","name":"Boolean"}}]}}})",
                "introspection: flatten, marker");
      }

      void rows()
      {
         expect(run(R"({ getAccount(accountName: "a30") { accountNum authService resourceOwner created updated } getAccounts(accountNames: ["a0", "a5"]) { accountNum resourceOwner } })"),
                R"({"data":{"getAccount":{"accountNum":"a30","authService":"a1","resourceOwner":"a3","created":"2023-11-14T22:13:23Z","updated":"2023-11-14T22:13:20.750000Z"},"getAccounts":[{"accountNum":"a0","resourceOwner":null},null]}})",
                "rows by key");
         expect(run(R"({ getAccount(accountName: "bob") { accountNum } })"),
                R"({"errors":[{"message":"Expected value of type \"AccountNumber\", found \"bob\".","locations":[{"line":1,"column":27}]}]})",
                "custom scalar argument");
         expect(run(R"({ block(blockNum: 3) { blockNum size header { producer time } } none: block(blockNum: 99) { size } })"),
                R"({"data":{"block":{"blockNum":3,"size":30,"header":{"producer":"a0","time":"2023-11-14T22:13:23.000000Z"}},"none":null}})",
                "resolver object");
         expect(run(R"({ lastTransfer { block { blockNum } from amount __typename } pair { first { accountNum } second { port } __typename } height { value } label { value __typename } })"),
                R"({"data":{"lastTransfer":{"block":{"blockNum":null},"from":"a5","amount":-3,"__typename":"TransferEventWithBlockContext"},"pair":{"first":{"accountNum":"a0"},"second":{"port":1},"__typename":"Pair_Account_IPV4Endpoint"},"height":{"value":30},"label":{"value":"main","__typename":"Label"}}})",
                "flatten, template names");
         // The flattened member read through a view of the encoded record.
         expect(run(R"({ storedEvent { event block { blockNum blockTime } inviteId actor __typename } })"),
                R"({"data":{"storedEvent":{"event":"stored","block":{"blockNum":9,"blockTime":null},"inviteId":4,"actor":"a44","__typename":"InviteEventWithBlockContext"}}})",
                "flatten through a view");
         expect_has(run(R"({ storedEvent { data { inviteId } } })"),
                    R"(Cannot query field \"data\" on type \"InviteEventWithBlockContext\".)", "flattened member is not a field");
      }

      void ranges_and_pages()
      {
         constexpr std::string_view info = " pageInfo { hasNextPage hasPreviousPage startCursor endCursor } } }";
         // An index returned as it is: the field has the range and the page.
         db.reads = 0;
         expect(run(std::string{R"({ accounts(ge: "a20", lt: "a60", first: 2) { edges { cursor node { accountNum } })"} + std::string{info}),
                R"({"data":{"accounts":{"edges":[{"cursor":"0000000000000014","node":{"accountNum":"a20"}},{"cursor":"000000000000001E","node":{"accountNum":"a30"}}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"startCursor":"0000000000000014","endCursor":"000000000000001E"}}}})",
                "range, first");
         // (Twice: once for each schema `run` asks.) Two rows a page, no more.
         expect_true(db.reads == 4, "a page reads its rows only");
         constexpr std::string_view brief = " pageInfo { hasNextPage hasPreviousPage } } }";
         auto page = [&](std::string_view args) {
            return run("{ accounts(" + std::string{args} + ") { edges { node { accountNum } }" + std::string{brief});
         };
         auto is = [&](std::string_view args, std::string_view names, bool next, bool previous) {
            std::string want = R"({"data":{"accounts":{"edges":[)";
            for (std::size_t at = 0; at < names.size();)
            {
               const std::size_t end = std::min(names.find(' ', at), names.size());
               want += std::string{at ? "," : ""} + R"({"node":{"accountNum":")" + std::string{names.substr(at, end - at)} + R"("}})";
               at = end + 1;
            }
            want += std::string{R"(],"pageInfo":{"hasNextPage":)"} + (next ? "true" : "false") +
                    R"(,"hasPreviousPage":)" + (previous ? "true" : "false") + "}}}}";
            expect(page(args), want, args);
         };
         is(R"(ge: "a20", lt: "a60", first: 2, after: "000000000000001e")", "a40 a50", false, true);
         is(R"(gt: "a20", le: "a40")", "a30 a40", false, false);
         is(R"(ge: "a20", lt: "a60", last: 2)", "a40 a50", false, true);
         is(R"(ge: "a20", lt: "a60", last: 2, before: "0000000000000028")", "a20 a30", true, false);
         // A cursor outside the range does not widen it.
         is(R"(ge: "a20", after: "0000000000000000", first: 1)", "a20", true, false);
         is(R"(lt: "a20", before: "0000000000000064", last: 1)", "a10", false, true);
         // Both cursors and both bounds.
         is(R"(ge: "a10", le: "a90", after: "0000000000000014", before: "0000000000000046", first: 3)", "a30 a40 a50", true, true);
         // Bounds between keys, empty ranges, a bound beyond the last key.
         is(R"(gt: "a15", lt: "a45")", "a20 a30 a40", false, false);
         is(R"(le: "a15", last: 1)", "a10", false, true);
         is(R"(gt: "a110")", "", false, false);
         is(R"(ge: "a50", lt: "a20")", "", false, false);
         is(R"(ge: "a50", lt: "a20", last: 3)", "", false, false);
         is(R"(ge: "a105", first: 5)", "a110", false, false);
         // The tighter of two bounds on one side.
         is(R"(gt: "a20", ge: "a40", lt: "a70", le: "a50")", "a40 a50", false, false);
         is(R"(gt: "a40", ge: "a20", lt: "a60", le: "a90")", "a50", false, false);
         // Variables fill the arguments of a group.
         expect(run("query ($from: AccountNumber, $n: Int, $c: String) { accounts(ge: $from, first: $n, after: $c) { edges { node { accountNum } } } }",
                    R"({"from": "a90", "n": 2, "c": null})"),
                R"({"data":{"accounts":{"edges":[{"node":{"accountNum":"a90"}},{"node":{"accountNum":"a100"}}]}}})",
                "variables");
         expect_has(run("query ($from: AccountNumber) { accounts(ge: $from) { edges { cursor } } }", R"({"from": "x"})"),
                    R"(Variable \"$from\" got invalid value)", "variable of a custom scalar");
         expect(run(R"({ accountsFrom(start: "a100") { edges { node { accountNum } } } })"),
                R"({"data":{"accountsFrom":{"edges":[{"node":{"accountNum":"a100"}},{"node":{"accountNum":"a110"}}]}}})",
                "a range fixed by the service");
         // Invalid arguments are field errors.
         expect(run("{ accounts(first: 1, last: 1) { edges { cursor } } }"),
                R"({"data":{"accounts":null},"errors":[{"message":"Arguments \"first\" and \"last\" cannot both be given.","locations":[{"line":1,"column":3}],"path":["accounts"]}]})",
                "first and last");
         expect_has(run(R"({ accounts(after: "zz") { edges { cursor } } })"), R"(Argument \"after\" is not a cursor.)", "cursor");
         expect_has(run(R"({ accounts(ge: 7) { edges { cursor } } })"), R"(Expected value of type \"AccountNumber\", found 7.)",
                    "bound of the key's type");
         // `first` inside a group is the field's page size for the complexity limit.
         gql::limits lim;
         lim.max_complexity = 200;
         expect_has(run("{ accounts(first: 100) { edges { node { accountNum } } } }", {}, lim),
                    "Query complexity exceeds the limit of 200.", "complexity");
         expect_has(run("{ accounts(first: 5) { edges { node { accountNum } } } }", {}, lim), R"("accountNum":"a40")",
                    "complexity within the limit");
      }

      void transformed_and_views()
      {
         expect(run(R"({ peers(first: 3) { edges { node { id urls tls { _ } endpoint { __typename ... on IPV4Endpoint { address port } ... on LocalEndpoint { _ } } secure url(index: 1) } } pageInfo { hasNextPage } } })"),
                R"({"data":{"peers":{"edges":[{"node":{"id":1,"urls":["https://peer1.example","wss://peer1"],"tls":{"_":true},"endpoint":{"__typename":"IPV4Endpoint","address":"10.0.0.1","port":8080},"secure":true,"url":"wss://peer1"}},{"node":{"id":2,"urls":["https://peer2.example","wss://peer2"],"tls":null,"endpoint":{"__typename":"IPV4Endpoint","address":"10.0.0.2","port":8080},"secure":false,"url":"wss://peer2"}},{"node":{"id":3,"urls":["https://peer3.example","wss://peer3"],"tls":{"_":true},"endpoint":{"__typename":"LocalEndpoint","_":true},"secure":true,"url":"wss://peer3"}}],"pageInfo":{"hasNextPage":true}}}})",
                "transformed rows");
         expect(run(R"({ peerRows(last: 3) { edges { cursor node { socket tls { _ __typename } endpoint { __typename ... on IPV4Endpoint { port } ... on LocalEndpoint { _ } } } } } })"),
                R"({"data":{"peerRows":{"edges":[{"cursor":"80000003","node":{"socket":3,"tls":{"_":true,"__typename":"TLSInfo"},"endpoint":{"__typename":"LocalEndpoint","_":true}}},{"cursor":"80000004","node":{"socket":4,"tls":null,"endpoint":{"__typename":"IPV4Endpoint","port":8080}}},{"cursor":"80000005","node":{"socket":5,"tls":{"_":true,"__typename":"TLSInfo"},"endpoint":{"__typename":"IPV4Endpoint","port":8080}}}]}}})",
                "markers and a union through views");
         // The cursors of transformed rows are the index's.
         const std::string first = run("{ peers(first: 2) { edges { cursor } pageInfo { endCursor } } }");
         expect(run("{ peers(first: 2, after: \"" + string_of(first, "endCursor") + "\") { edges { node { id } } pageInfo { hasPreviousPage } } }"),
                R"({"data":{"peers":{"edges":[{"node":{"id":3}},{"node":{"id":4}}],"pageInfo":{"hasPreviousPage":true}}}})",
                "paging transformed rows");
      }

      void byte_budget()
      {
         // Rows of 38, 48, 58, ... bytes (10 more each); a page has at most
         // 256 bytes of them, and at least one row.
         std::vector<std::size_t> sizes;
         for (const auto& [key, row] : db.blocks)
            sizes.push_back(row.size());
         std::vector<std::uint32_t> seen;
         std::string                after;
         std::size_t                pages = 0;
         while (pages < 100)
         {
            const std::string out = run("{ blocks(" + (after.empty() ? std::string{} : "after: \"" + after + "\", ") +
                                        "first: 500) { edges { node { blockNum } } pageInfo { hasNextPage endCursor } } }");
            const auto        nums = block_nums(out);
            std::size_t       bytes = 0;
            for (const std::uint32_t n : nums)
               bytes += sizes[n - 1];
            expect_true(!nums.empty() && (nums.size() == 1 || bytes <= 256), "a page fits the budget, or is one row");
            // ... and is as long as the budget allows.
            const std::size_t next = nums.empty() ? 0 : nums.back();  // index of the row after the page
            expect_true(next == sizes.size() || bytes + sizes[next] > 256, "a page is not cut short");
            seen.insert(seen.end(), nums.begin(), nums.end());
            ++pages;
            if (out.find(R"("hasNextPage":true)") == std::string::npos)
               break;
            after = string_of(out, "endCursor");
         }
         std::vector<std::uint32_t> all;
         for (std::uint32_t n = 1; n <= sizes.size(); ++n)
            all.push_back(n);
         expect_true(seen == all, "the pages cover every row once");
         // 1-4, 5-6, 7-8, 9-10, then one row a page: two of them exceed it.
         expect_true(pages == 24, "pages under the budget");
         expect(run("{ blocks { edges { node { blockNum } } pageInfo { hasNextPage endCursor } } }"),
                R"({"data":{"blocks":{"edges":[{"node":{"blockNum":1}},{"node":{"blockNum":2}},{"node":{"blockNum":3}},{"node":{"blockNum":4}}],"pageInfo":{"hasNextPage":true,"endCursor":"00000004"}}}})",
                "budget, forward");
         // Backward, and with a range: a row larger than the budget is a page.
         expect(run("{ a: blocks(ge: 26, le: 28) { edges { node { blockNum } } pageInfo { hasNextPage hasPreviousPage } } b: blocks(last: 50, lt: 6) { edges { node { blockNum } } pageInfo { hasNextPage hasPreviousPage } } }"),
                R"({"data":{"a":{"edges":[{"node":{"blockNum":26}}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false}},"b":{"edges":[{"node":{"blockNum":2}},{"node":{"blockNum":3}},{"node":{"blockNum":4}},{"node":{"blockNum":5}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":true}}}})",
                "budget, range and backward");
      }

      void built_by_hand()
      {
         expect(run("{ history(inviteId: 1, first: 2) { edges { cursor node { block { blockNum blockTime } inviteId actor event } } pageInfo { hasNextPage hasPreviousPage startCursor endCursor } } }"),
                R"({"data":{"history":{"edges":[{"cursor":"0000000000000002","node":{"block":{"blockNum":0,"blockTime":"2023-11-14T22:13:20.000000Z"},"inviteId":1,"actor":"a101","event":"accepted"}},{"cursor":"0000000000000004","node":{"block":{"blockNum":1,"blockTime":"2023-11-14T22:13:21.000000Z"},"inviteId":1,"actor":"a103","event":"created"}}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false,"startCursor":"0000000000000002","endCursor":"0000000000000004"}}}})",
                "a connection built by hand");
         expect(run(R"({ history(inviteId: 1, first: 2, after: "0000000000000004") { edges { cursor node { actor __typename } } pageInfo { hasNextPage hasPreviousPage } } })"),
                R"({"data":{"history":{"edges":[{"cursor":"0000000000000006","node":{"actor":"a105","__typename":"InviteEventWithBlockContext"}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":true}}}})",
                "its next page");
         expect(run("{ history(inviteId: 1, last: 2) { edges { cursor } } }"),
                R"({"data":{"history":null},"errors":[{"message":"history pages forward only.","locations":[{"line":1,"column":3}],"path":["history"]}]})",
                "a field error from the service");
      }

      // A compiled plan serves any number of requests and roots.
      void plans()
      {
         gql::document           plan;
         std::vector<gql::error> errors;
         const std::string_view  text =
            "query ($from: AccountNumber, $n: Int) { accounts(ge: $from, first: $n) { edges { node { accountNum updated } } pageInfo { hasNextPage } } }";
         if (!api::compile(text, plan, errors))
         {
            failures.push_back("plan does not compile");
            return;
         }
         const Db other = make_db(3);
         for (const std::string_view variables : {R"({"from": "a90", "n": 2})", R"({"n": 1})", R"({"from": "a0"})"})
         {
            std::string out;
            api::execute(plan, Query{&db}, out, gql::request{.variables = variables});
            expect(out, run(text, variables), "plan against text");
            out.clear();
            api::execute(plan, Query{&other}, out, gql::request{.variables = variables});
            expect_true(out.find("\"data\":{\"accounts\":{\"edges\":[") != std::string::npos, "plan on another root");
         }
      }

      std::vector<std::string> all()
      {
         schema();
         rows();
         ranges_and_pages();
         transformed_and_views();
         byte_budget();
         built_by_hand();
         plans();
         return failures;
      }
   };

   inline std::vector<std::string> service_failures()
   {
      return checker{}.all();
   }
}  // namespace gqls
