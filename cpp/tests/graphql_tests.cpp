// psio::graphql — parse, validate, plan, execute (docs/graphql.md).
//
// The suite follows the GraphQL October 2021 specification section by
// section: every validation rule of section 5 has a failing document with
// the expected message and position, every execution rule of section 6 a
// query with the expected response text. The schema is graphql_fixture.hpp.

#include "graphql_fixture.hpp"

#include <catch.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace gqlt;
namespace gql = psio::graphql;

namespace
{
   bool json_valid(std::string_view text)
   {
      psio::json_text_cursor c{std::span<const char>{text.data(), text.size()}, true};
      return c.skip() && c.finish();
   }

   std::string run(const Query& q, std::string_view text, gql::request req = {}, const gql::limits& lim = {})
   {
      std::string out;
      api::run(q, text, out, req, lim);
      REQUIRE(json_valid(out));
      return out;
   }
   std::string run(std::string_view text, std::string_view variables = {})
   {
      static const Query q = make_query();
      return run(q, text, gql::request{.variables = variables});
   }

   // A document with `~` in front of the token an error must point at.
   struct marked
   {
      std::string   text;
      std::uint32_t line = 0, column = 0;
      explicit marked(std::string_view in)
      {
         std::uint32_t l = 1, c = 1;
         for (const char ch : in)
         {
            if (ch == '~')
            {
               line   = l;
               column = c;
               continue;
            }
            text.push_back(ch);
            if (ch == '\n')
            {
               ++l;
               c = 1;
            }
            else
               ++c;
         }
      }
   };

   // Compiles `in`; requires an error with `message` at the marked position.
   void rejects(std::string_view in, std::string_view message)
   {
      const marked            m{in};
      gql::document           plan;
      std::vector<gql::error> errors;
      INFO(m.text);
      REQUIRE_FALSE(api::compile(m.text, plan, errors));
      bool found = false;
      std::string seen;
      for (const gql::error& e : errors)
      {
         seen += std::to_string(e.line) + ":" + std::to_string(e.column) + " " + e.message + "\n";
         found = found || (e.message == message && e.line == m.line && e.column == m.column);
      }
      INFO("expected " << m.line << ":" << m.column << " " << message << "\ngot\n" << seen);
      REQUIRE(found);
   }
   void accepts(std::string_view text)
   {
      gql::document           plan;
      std::vector<gql::error> errors;
      INFO(text);
      const bool ok = api::compile(text, plan, errors);
      INFO((errors.empty() ? std::string{} : errors[0].message));
      REQUIRE(ok);
   }

   // ── A small JSON tree for the introspection round trip ────────────────
   struct J
   {
      psio::json_kind                        kind = psio::json_kind::null;
      bool                                   flag = false;
      std::string                            text;
      std::vector<J>                         items;
      std::vector<std::pair<std::string, J>> members;

      const J& operator[](std::string_view key) const
      {
         static const J none;
         for (const auto& [k, v] : members)
            if (k == key)
               return v;
         return none;
      }
      bool is_null() const { return kind == psio::json_kind::null; }
   };
   bool read_json(psio::json_text_cursor& c, J& out)
   {
      if (!c.kind(out.kind))
         return false;
      bool             more = false, escaped = false;
      std::string_view text;
      switch (out.kind)
      {
         case psio::json_kind::null: return c.null();
         case psio::json_kind::boolean: return c.boolean(out.flag);
         case psio::json_kind::number:
            if (!c.number(text))
               return false;
            out.text = text;
            return true;
         case psio::json_kind::string:
            if (!c.string(text, escaped))
               return false;
            out.text = text;
            return true;
         case psio::json_kind::array:
            if (!c.begin_array())
               return false;
            while (c.next_element(more) && more)
               if (!read_json(c, out.items.emplace_back()))
                  return false;
            return !more;
         default:
            if (!c.begin_object())
               return false;
            while (c.next_field(more, text) && more)
            {
               std::string key{text};
               if (!read_json(c, out.members.emplace_back(std::move(key), J{}).second))
                  return false;
            }
            return !more;
      }
   }
   J parse_json(std::string_view text)
   {
      psio::json_text_cursor c{std::span<const char>{text.data(), text.size()}, true};
      J                      out;
      REQUIRE(read_json(c, out));
      return out;
   }

   // What graphql-js's getIntrospectionQuery() sends (GraphiQL, codegen).
   constexpr std::string_view introspection_query = R"(
    query IntrospectionQuery {
      __schema {
        description
        queryType { name }
        mutationType { name }
        subscriptionType { name }
        types { ...FullType }
        directives { name description locations args { ...InputValue } }
      }
    }
    fragment FullType on __Type {
      kind
      name
      description
      specifiedByURL
      fields(includeDeprecated: true) {
        name
        description
        args { ...InputValue }
        type { ...TypeRef }
        isDeprecated
        deprecationReason
      }
      inputFields { ...InputValue }
      interfaces { ...TypeRef }
      enumValues(includeDeprecated: true) { name description isDeprecated deprecationReason }
      possibleTypes { ...TypeRef }
    }
    fragment InputValue on __InputValue { name description type { ...TypeRef } defaultValue }
    fragment TypeRef on __Type {
      kind name
      ofType { kind name ofType { kind name ofType { kind name ofType { kind name ofType { kind name
        ofType { kind name ofType { kind name ofType { kind name ofType { kind name } } } } } } } } }
    }
   )";

   std::string type_ref_text(const J& t)
   {
      if (t["kind"].text == "NON_NULL")
         return type_ref_text(t["ofType"]) + "!";
      if (t["kind"].text == "LIST")
         return "[" + type_ref_text(t["ofType"]) + "]";
      return t["name"].text;
   }
   std::string quoted(const std::string& s)
   {
      std::string out = "\"";
      for (const char c : s)
      {
         if (c == '"' || c == '\\')
            out += '\\';
         if (c == '\n')
            out += "\\n";
         else
            out += c;
      }
      return out + "\"";
   }
   // SDL from an introspection result, in the layout of psio::graphql::sdl.
   std::string sdl_from_introspection(const J& schema)
   {
      std::string       out;
      const std::string query    = schema["queryType"]["name"].text;
      const J&          mutation = schema["mutationType"];
      if (query != "Query" || (!mutation.is_null() && mutation["name"].text != "Mutation"))
      {
         out += "schema {\n  query: " + query;
         if (!mutation.is_null())
            out += "\n  mutation: " + mutation["name"].text;
         out += "\n}\n\n";
      }
      auto describe = [&](const J& d, const char* indent) {
         if (!d.is_null())
            out += indent + quoted(d.text) + "\n";
      };
      auto args = [&](const J& list) {
         if (list.items.empty())
            return;
         out += "(";
         for (std::size_t i = 0; i < list.items.size(); ++i)
            out += (i ? ", " : "") + list.items[i]["name"].text + ": " + type_ref_text(list.items[i]["type"]);
         out += ")";
      };
      for (const J& t : schema["types"].items)
      {
         const std::string name = t["name"].text;
         const std::string kind = t["kind"].text;
         if (name.starts_with("__") || name == "Int" || name == "Float" || name == "String" || name == "Boolean" ||
             name == "ID")
            continue;
         describe(t["description"], "");
         if (kind == "SCALAR")
            out += "scalar " + name + "\n";
         else if (kind == "ENUM")
         {
            out += "enum " + name + " {\n";
            for (const J& v : t["enumValues"].items)
               out += "  " + v["name"].text + "\n";
            out += "}\n";
         }
         else if (kind == "UNION")
         {
            out += "union " + name + " = ";
            for (std::size_t i = 0; i < t["possibleTypes"].items.size(); ++i)
               out += (i ? " | " : "") + t["possibleTypes"].items[i]["name"].text;
            out += "\n";
         }
         else if (kind == "INPUT_OBJECT")
         {
            out += "input " + name + " {\n";
            for (const J& f : t["inputFields"].items)
            {
               describe(f["description"], "  ");
               out += "  " + f["name"].text + ": " + type_ref_text(f["type"]) + "\n";
            }
            out += "}\n";
         }
         else
         {
            out += "type " + name + " {\n";
            for (const J& f : t["fields"].items)
            {
               describe(f["description"], "  ");
               out += "  " + f["name"].text;
               args(f["args"]);
               out += ": " + type_ref_text(f["type"]);
               if (f["isDeprecated"].flag)
                  out += " @deprecated(reason: " + quoted(f["deprecationReason"].text) + ")";
               out += "\n";
            }
            out += "}\n";
         }
         out += "\n";
      }
      if (!out.empty())
         out.pop_back();
      return out;
   }
}  // namespace

// ── Section 2: language ─────────────────────────────────────────────────

TEST_CASE("graphql: syntax errors carry a location", "[graphql][parse]")
{
   rejects("~", "Syntax Error: Unexpected <EOF>.");
   rejects("{~", "Syntax Error: Expected Name, found <EOF>.");
   rejects("{ nothing ~", "Syntax Error: Expected Name, found <EOF>.");
   rejects("{ nothing } ~}", "Syntax Error: Expected a definition, found \"}\".");
   rejects("{ echo(text: ~) }", "Syntax Error: Expected a value, found \")\".");
   rejects("{ echo(text: ~\"open) }", "Syntax Error: Unterminated string.");
   rejects("{ add(a: ~01, b: 1) }", "Syntax Error: Invalid number, unexpected digit after 0.");
   rejects("{ half(value: ~1.) }", "Syntax Error: Invalid number, expected digit after \".\".");
   rejects("{ half(value: ~1e) }", "Syntax Error: Invalid number, expected digit in exponent.");
   rejects("{ add(a: ~1x, b: 1) }", "Syntax Error: Invalid number, unexpected character after it.");
   rejects("{ echo(text: \"a~\\q\") }", "Syntax Error: Invalid character escape sequence.");
   rejects("{ echo(text: \"~\\ud800\") }", "Syntax Error: Invalid Unicode escape sequence.");
   rejects("{ echo(text: \"~\\u12G4\") }", "Syntax Error: Invalid Unicode escape sequence.");
   rejects("{ echo(text: ~\"\xff\") }", "Syntax Error: Invalid UTF-8 within String.");
   rejects("{ nothing ~? }", "Syntax Error: Unexpected character.");
   rejects("{ nothing ~.. }", "Syntax Error: Unexpected character \".\".");
   rejects("{ ... ~}", "Syntax Error: Expected \"{\", found \"}\".");
   rejects("fragment ~on on Query { nothing }", "Syntax Error: Expected a fragment name, found \"on\".");
   rejects("query ($a: Int = ~$b) { nothing }", "Syntax Error: Unexpected variable in constant value.");
   rejects("{\n  nothing\n  echo(text: ~)\n}", "Syntax Error: Expected a value, found \")\".");
   rejects("{ nothing }\r\n{ ~", "Syntax Error: Expected Name, found <EOF>.");
   rejects("~type Foo { a: Int }",
           "Only executable definitions (operations and fragments) are allowed in a request.");
   rejects("~\"description\" type Foo { a: Int }",
           "Only executable definitions (operations and fragments) are allowed in a request.");
}

TEST_CASE("graphql: ignored tokens, names, numbers, strings", "[graphql][parse]")
{
   // Commas, comments, a byte order mark and whitespace are insignificant.
   CHECK(run("\xEF\xBB\xBF{,,nothing,# comment } \n,echo(text:\"a\",),}") ==
         R"({"data":{"nothing":null,"echo":"a"}})");
   // String escapes, including a surrogate pair, and UTF-8 passed through.
   CHECK(run(R"({ echo(text: "q\"b\\s\/\b\f\n\r\té😀 é") })") ==
         "{\"data\":{\"echo\":\"q\\\"b\\\\s/\\u0008\\u000c\\n\\r\\t\xC3\xA9\xF0\x9F\x98\x80 \xC3\xA9\"}}");
   // Block strings: common indentation and blank edge lines are removed.
   CHECK(run("{ echo(text: \"\"\"\n    Hello,\n      World!\n\n    Yours,\n      GraphQL. \\\"\"\" \"\n  \"\"\") }") ==
         R"({"data":{"echo":"Hello,\n  World!\n\nYours,\n  GraphQL. \"\"\" \""}})");
   CHECK(run(R"({ echo(text: """one""") })") == R"({"data":{"echo":"one"}})");
   CHECK(run(R"({ echo(text: "") e2: echo(text: """""") })") == R"({"data":{"echo":"","e2":""}})");
   // Numbers.
   CHECK(run("{ add(a: -0, b: 2147483647) half(value: 1e2) h2: half(value: -0.5E+1) h3: half(value: 3) }") ==
         R"({"data":{"add":2147483647,"half":50,"h2":-2.5,"h3":1.5}})");
}

// ── Section 5: validation ───────────────────────────────────────────────

TEST_CASE("graphql: 5.2 operations", "[graphql][validate]")
{
   rejects("query A { nothing } ~query A { nothing }", "There can be only one operation named \"A\".");
   rejects("~{ nothing } query B { nothing }", "This anonymous operation must be the only defined operation.");
   rejects("~subscription { nothing }", "Subscriptions are not supported.");
   accepts("query A { nothing } query B { nothing } mutation C { value }");
}

TEST_CASE("graphql: 5.3 fields", "[graphql][validate]")
{
   rejects("{ ~nope }", "Cannot query field \"nope\" on type \"Query\".");
   rejects("{ node { ~secret } }", "Cannot query field \"secret\" on type \"Node\".");
   rejects("{ node { ~renamed_member } }", "Cannot query field \"renamed_member\" on type \"Node\".");
   rejects("{ search { ~label } }",
           "Cannot query field \"label\" on type \"Tag_or_Extra\". Did you mean to use an inline fragment on "
           "one of its member types?");
   rejects("{ node { ~__schema { queryType { name } } } }", "Cannot query field \"__schema\" on type \"Node\".");
   rejects("mutation { ~__schema { queryType { name } } }", "Cannot query field \"__schema\" on type \"Mutation\".");
   // Leaf field selections (5.3.3).
   rejects("{ ~nothing { x } }", "Field \"nothing\" must not have a selection since type \"Int\" has no subfields.");
   rejects("{ items { ~status { x } } }",
           "Field \"status\" must not have a selection since type \"Status!\" has no subfields.");
   rejects("{ ~node }", "Field \"node\" of type \"Node!\" must have a selection of subfields. Did you mean \"node { ... }\"?");
   rejects("{ ~search }",
           "Field \"search\" of type \"[Tag_or_Extra!]!\" must have a selection of subfields. Did you mean "
           "\"search { ... }\"?");
   accepts("{ search { __typename } node { __typename } __typename }");
}

TEST_CASE("graphql: 5.3.2 field selection merging", "[graphql][validate]")
{
   rejects("{ a: echo(text: \"1\") ~a: echo(text: \"2\") }",
           "Fields \"a\" conflict because they have differing arguments. Use different aliases on the fields to "
           "fetch both if this was intentional.");
   rejects("{ echo(text: \"1\") ...F } fragment F on Query { ~echo(text: \"2\") }",
           "Fields \"echo\" conflict because they have differing arguments. Use different aliases on the fields "
           "to fetch both if this was intentional.");
   rejects("{ a: nothing ~a: nan }",
           "Fields \"a\" conflict because \"nothing\" and \"nan\" are different fields. Use different aliases on "
           "the fields to fetch both if this was intentional.");
   rejects("query ($x: String!, $y: String!) { echo(text: $x) ~echo(text: $y) }",
           "Fields \"echo\" conflict because they have differing arguments. Use different aliases on the fields "
           "to fetch both if this was intentional.");
   rejects("{ describe(filter: {names: [\"a\"]}) ~describe(filter: {names: [\"b\"]}) }",
           "Fields \"describe\" conflict because they have differing arguments. Use different aliases on the "
           "fields to fetch both if this was intentional.");
   // Members of a union: the same response key must have the same shape.
   rejects("{ search { ... on Tag { v: label } ... on Extra { ~v: codes } } }",
           "Fields \"v\" conflict because they return conflicting types \"String!\" and \"[Int!]!\". Use "
           "different aliases on the fields to fetch both if this was intentional.");
   rejects("{ search { ... on Tag { v: weight } ... on Extra { ~v: text } } }",
           "Fields \"v\" conflict because they return conflicting types \"Int!\" and \"String!\". Use different "
           "aliases on the fields to fetch both if this was intentional.");
   // Identical fields merge; different fields of union members may share a
   // key when their shapes agree; input object member order is irrelevant.
   accepts("{ echo(text: \"1\") echo(text: \"1\") }");
   accepts("query ($x: String!) { echo(text: $x) echo(text: $x) }");
   accepts("{ search { ... on Tag { v: label } ... on Extra { v: text } } }");
   accepts("{ describe(filter: {names: [], minCount: 1}) describe(filter: {minCount: 1, names: []}) }");
   CHECK(run("{ items { id } items { count } ...F } fragment F on Query { items { id flag } }")
            .starts_with(R"({"data":{"items":[{"id":1000,"count":-2,"flag":true},)"));
}

TEST_CASE("graphql: 5.4 arguments", "[graphql][validate]")
{
   rejects("{ echo(text: \"a\", ~bogus: 1) }", "Unknown argument \"bogus\" on field \"Query.echo\".");
   rejects("{ nothing(~a: 1) }", "Unknown argument \"a\" on field \"Query.nothing\".");
   rejects("{ __typename(~a: 1) }", "Unknown argument \"a\" on field \"Query.__typename\".");
   rejects("{ echo(text: \"a\", ~text: \"b\") }", "There can be only one argument named \"text\".");
   rejects("{ ~echo }", "Field \"echo\" argument \"text\" of type \"String!\" is required, but it was not provided.");
   rejects("{ ~add(a: 1) }", "Field \"add\" argument \"b\" of type \"Int!\" is required, but it was not provided.");
   rejects("{ ~__type { name } }",
           "Field \"__type\" argument \"name\" of type \"String!\" is required, but it was not provided.");
   accepts("{ maybe }");
}

TEST_CASE("graphql: 5.5 fragments", "[graphql][validate]")
{
   rejects("{ ...F } fragment F on Query { nothing } ~fragment F on Query { nothing }",
           "There can be only one fragment named \"F\".");
   rejects("{ ...F } fragment F on ~Nope { nothing }", "Unknown type \"Nope\".");
   rejects("{ ~... on Nope { nothing } }", "Unknown type \"Nope\".");
   rejects("{ ...F } fragment F on ~Status { nothing }",
           "Fragment \"F\" cannot condition on non composite type \"Status\".");
   rejects("{ ...F } fragment F on ~FilterInput { nothing }",
           "Fragment \"F\" cannot condition on non composite type \"FilterInput\".");
   rejects("{ ~... on Int { nothing } }", "Fragment cannot condition on non composite type \"Int\".");
   rejects("{ nothing } ~fragment F on Query { nothing }", "Fragment \"F\" is never used.");
   rejects("{ ~...Missing }", "Unknown fragment \"Missing\".");
   rejects("{ ...A } fragment A on Query { ...B } fragment B on Query { ~...A }",
           "Cannot spread fragment \"A\" within itself.");
   rejects("{ ...A } fragment A on Query { nothing ~...A }", "Cannot spread fragment \"A\" within itself.");
   rejects("{ ~... on Tag { label } }",
           "Fragment cannot be spread here as objects of type \"Query\" can never be of type \"Tag\".");
   rejects("{ ~...F } fragment F on Tag { label }",
           "Fragment \"F\" cannot be spread here as objects of type \"Query\" can never be of type \"Tag\".");
   rejects("{ search { ~... on Item { id } } }",
           "Fragment cannot be spread here as objects of type \"Tag_or_Extra\" can never be of type \"Item\".");
   rejects("{ items { detail { ... on Tag { ~... on Extra { text } } } } }",
           "Fragment cannot be spread here as objects of type \"Tag\" can never be of type \"Extra\".");
   // A union in an object scope and the reverse are possible spreads.
   accepts("{ search { ... on Tag_or_Extra { ... on Tag { label } } } }");
   accepts("{ search { ... on Tag { ... on Tag_or_Extra { __typename } } } }");
   accepts("{ ... { nothing } ... on Query { nothing } ...F } fragment F on Query { ...G } fragment G on Query { nan }");
}

TEST_CASE("graphql: 5.6 values", "[graphql][validate]")
{
   rejects("{ add(a: ~\"1\", b: 2) }", "Expected value of type \"Int\", found \"1\".");
   rejects("{ add(a: ~1.5, b: 2) }", "Expected value of type \"Int\", found 1.5.");
   rejects("{ add(a: ~4294967296, b: 2) }", "Expected value of type \"Int\", found 4294967296.");
   rejects("{ add(a: ~true, b: 2) }", "Expected value of type \"Int\", found true.");
   rejects("{ add(a: ~[1, 2], b: 2) }", "Expected value of type \"Int\", found a list.");
   rejects("{ add(a: ~{x: 1}, b: 2) }", "Expected value of type \"Int\", found an object.");
   rejects("{ add(a: ~active, b: 2) }", "Expected value of type \"Int\", found active.");
   rejects("{ add(a: ~null, b: 1) }", "Expected value of type \"Int!\", found null.");
   // The check is the C++ parameter's: an int8_t argument holds -128..127.
   rejects("{ tiny(v: ~300) }", "Expected value of type \"Int\", found 300.");
   rejects("{ echo(text: ~1) }", "Expected value of type \"String\", found 1.");
   rejects("{ echo(text: ~word) }", "Expected value of type \"String\", found word.");
   rejects("{ half(value: ~true) }", "Expected value of type \"Float\", found true.");
   rejects("{ half(value: ~\"1.5\") }", "Expected value of type \"Float\", found \"1.5\".");
   rejects("{ risky(ok: ~1) }", "Expected value of type \"Boolean\", found 1.");
   rejects("{ flip(status: ~\"active\") }", "Expected value of type \"Status\", found \"active\".");
   rejects("{ flip(status: ~bogus) }", "Value \"bogus\" does not exist in \"Status\" enum.");
   rejects("{ flip(status: ~1) }", "Expected value of type \"Status\", found 1.");
   // Custom scalars: the value must be one psio's I-JSON reader accepts.
   rejects("{ big(value: ~\"x\") }", "Expected value of type \"UInt64\", found \"x\".");
   rejects("{ big(value: ~-1) }", "Expected value of type \"UInt64\", found -1.");
   rejects("{ big(value: ~1.5) }", "Expected value of type \"UInt64\", found 1.5.");
   rejects("{ big(value: ~18446744073709551616) }", "Expected value of type \"UInt64\", found 18446744073709551616.");
   rejects("{ later(at: ~\"yesterday\") }", "Expected value of type \"DateTimeSeconds\", found \"yesterday\".");
   rejects("{ later(at: ~5) }", "Expected value of type \"DateTimeSeconds\", found 5.");
   rejects("{ owner(name: ~\"bob\") }", "Expected value of type \"AccountName\", found \"bob\".");
   rejects("{ bytes(data: ~\"0g\") }", "Expected value of type \"Bytes\", found \"0g\".");
   rejects("{ bytes(data: ~[1, 2]) }", "Expected value of type \"Bytes\", found a list.");
   // Lists: every item is checked; one item coerces to a list of it.
   rejects("{ ints(values: [1, ~\"a\"]) }", "Expected value of type \"Int\", found \"a\".");
   rejects("{ ints(values: [1, ~null]) }", "Expected value of type \"Int!\", found null.");
   rejects("{ total(grid: [[1], [~x]]) }", "Expected value of type \"Int\", found x.");
   accepts("{ ints(values: 1) total(grid: 4) t2: total(grid: [1, 2]) }");
   // Input objects (5.6.2 - 5.6.4).
   rejects("{ describe(filter: {names: [], ~bogus: 1}) }", "Field \"bogus\" is not defined by type \"FilterInput\".");
   rejects("{ describe(filter: {names: [], ~names: []}) }", "There can be only one input field named \"names\".");
   rejects("{ describe(filter: ~{status: active}) }",
           "Field \"FilterInput.names\" of required type \"[String!]!\" was not provided.");
   rejects("{ describe(filter: {names: [], range: ~{lo: 1}}) }",
           "Field \"RangeInput.hi\" of required type \"Int!\" was not provided.");
   rejects("{ describe(filter: ~1) }", "Expected value of type \"FilterInput\", found 1.");
   rejects("{ describe(filter: {names: [], status: ~3}) }", "Expected value of type \"Status\", found 3.");
}

TEST_CASE("graphql: 5.7 directives", "[graphql][validate]")
{
   rejects("{ nothing ~@bogus }", "Unknown directive \"@bogus\".");
   rejects("query ~@skip(if: true) { nothing }", "Directive \"@skip\" may not be used on QUERY.");
   rejects("mutation ~@include(if: true) { value }", "Directive \"@include\" may not be used on MUTATION.");
   rejects("{ ...F } fragment F on Query ~@skip(if: true) { nothing }",
           "Directive \"@skip\" may not be used on FRAGMENT_DEFINITION.");
   rejects("query ($a: String ~@include(if: true)) { maybe(text: $a) }",
           "Directive \"@include\" may not be used on VARIABLE_DEFINITION.");
   rejects("{ nothing ~@deprecated }", "Directive \"@deprecated\" may not be used on FIELD.");
   rejects("{ nothing @skip(if: true) ~@skip(if: false) }",
           "The directive \"@skip\" can only be used once at this location.");
   rejects("{ nothing ~@skip }",
           "Directive \"@skip\" argument \"if\" of type \"Boolean!\" is required, but it was not provided.");
   rejects("{ nothing @skip(~unless: true) }", "Unknown argument \"unless\" on directive \"@skip\".");
   rejects("{ nothing @skip(if: ~1) }", "Expected value of type \"Boolean\", found 1.");
   rejects("{ nothing @include(if: ~null) }", "Expected value of type \"Boolean!\", found null.");
   rejects("query ($v: Boolean) { nothing @include(if: ~$v) }",
           "Variable \"$v\" of type \"Boolean\" used in position expecting type \"Boolean!\".");
   accepts("{ nothing @skip(if: false) @include(if: true) ... @include(if: true) { nan } ...F @skip(if: true) } "
           "fragment F on Query { nothing }");
}

TEST_CASE("graphql: 5.8 variables", "[graphql][validate]")
{
   rejects("query ($a: String, ~$a: String) { maybe(text: $a) }", "There can be only one variable named \"$a\".");
   rejects("query ($a: ~Tag) { maybe(text: $a) }", "Variable \"$a\" cannot be non-input type \"Tag\".");
   rejects("query ($a: ~[Query!]) { maybe(text: $a) }", "Variable \"$a\" cannot be non-input type \"[Query!]\".");
   rejects("query ($a: ~Nope) { maybe(text: $a) }", "Unknown type \"Nope\".");
   rejects("{ maybe(text: ~$nope) }", "Variable \"$nope\" is not defined.");
   rejects("query Q { maybe(text: ~$nope) }", "Variable \"$nope\" is not defined by operation \"Q\".");
   rejects("query Q { ...F } fragment F on Query { maybe(text: ~$nope) }",
           "Variable \"$nope\" is not defined by operation \"Q\".");
   rejects("query Q(~$a: Int) { nothing }", "Variable \"$a\" is never used in operation \"Q\".");
   rejects("query (~$a: Int) { nothing }", "Variable \"$a\" is never used.");
   // A variable defined by one operation is not defined in another.
   rejects("query A($a: String) { maybe(text: $a) } query B { maybe(text: ~$a) }",
           "Variable \"$a\" is not defined by operation \"B\".");
   // Usages allowed (5.8.5).
   rejects("query ($a: Int) { add(a: ~$a, b: 1) }",
           "Variable \"$a\" of type \"Int\" used in position expecting type \"Int!\".");
   rejects("query ($a: String!) { add(a: ~$a, b: 1) }",
           "Variable \"$a\" of type \"String!\" used in position expecting type \"Int!\".");
   rejects("query ($a: Int!) { ints(values: ~$a) }",
           "Variable \"$a\" of type \"Int!\" used in position expecting type \"[Int!]!\".");
   rejects("query ($a: [Int]!) { ints(values: ~$a) }",
           "Variable \"$a\" of type \"[Int]!\" used in position expecting type \"[Int!]!\".");
   rejects("query ($a: Int = null) { add(a: ~$a, b: 1) }",
           "Variable \"$a\" of type \"Int\" used in position expecting type \"Int!\".");
   rejects("query ($a: Int) { describe(filter: {names: [], range: {lo: ~$a, hi: 2}}) }",
           "Variable \"$a\" of type \"Int\" used in position expecting type \"Int!\".");
   rejects("query ($a: Int = ~\"x\") { add(a: $a, b: 1) }", "Expected value of type \"Int\", found \"x\".");
   rejects("query ($a: FilterInput = ~{}) { describe(filter: $a) }",
           "Field \"FilterInput.names\" of required type \"[String!]!\" was not provided.");
   accepts("query ($a: Int = 3) { add(a: $a, b: 1) }");
   accepts("query ($a: Int!, $l: [Int!]!, $f: FilterInput!, $s: Status, $n: AccountName!, $t: DateTimeSeconds!) "
           "{ add(a: $a, b: $a) ints(values: $l) i2: ints(values: [$a, 2]) describe(filter: $f) "
           "d2: describe(filter: {names: [], status: $s}) owner(name: $n) later(at: $t) }");
}

TEST_CASE("graphql: validation reports several errors, each once", "[graphql][validate]")
{
   gql::document           plan;
   std::vector<gql::error> errors;
   // The fragment is expanded for both members of the union; its error is
   // reported once.
   REQUIRE_FALSE(api::compile("{ nope search { ...F } also } fragment F on Tag_or_Extra { __typename bad }", plan, errors));
   REQUIRE(errors.size() == 3);
   CHECK(errors[0].message == "Cannot query field \"nope\" on type \"Query\".");
   CHECK(errors[1].message == "Cannot query field \"also\" on type \"Query\".");
   CHECK(errors[2].message.starts_with("Cannot query field \"bad\" on type \"Tag_or_Extra\"."));
   std::string out;
   gql::write_error_response(errors, out);
   CHECK(out.starts_with(R"({"errors":[{"message":"Cannot query field \"nope\" on type \"Query\".","locations":[{"line":1,"column":3}]},)"));
   CHECK(json_valid(out));

   gql::limits few;
   few.max_errors = 2;
   REQUIRE_FALSE(api::compile("{ a b c d e }", plan, errors, few));
   CHECK(errors.size() == 2);
}

// ── Section 6: execution ────────────────────────────────────────────────

TEST_CASE("graphql: scalars are written as I-JSON writes them", "[graphql][execute]")
{
   CHECK(run("{ items { id name owner status count price flag blob created ttl note big small rev } }")
            .starts_with(R"({"data":{"items":[{"id":1000,"name":"item \"0\"\n","owner":"acct-0","status":"active",)"
                         R"("count":-2,"price":0,"flag":true,"blob":"00ABFF","created":"2023-11-14T22:13:20Z",)"
                         R"("ttl":0,"note":null,"big":0,"small":0,"rev":100},{"id":1001,"name":"item \"1\"\n",)"
                         R"("owner":"acct-1","status":"frozen","count":-1,"price":0.5,"flag":false,"blob":"01ABFF",)"
                         R"("created":"2023-11-14T22:13:21Z","ttl":1500,"note":"note 1",)"
                         R"("big":"-9007199254740993","small":7,"rev":null},{"id":"18446744073709551615",)"));
   // Every leaf of one item equals the I-JSON text of that member.
   const Query q    = make_query();
   const Item& item = q.items[1];
   auto        ijson = [](const auto& v) { return psio::encode(psio::ijson{}, v); };
   CHECK(run("{ item(id: 1001) { id name owner count price flag blob created ttl note labels range big small } }") ==
         "{\"data\":{\"item\":{\"id\":" + ijson(item.id) + ",\"name\":" + ijson(item.name) + ",\"owner\":" +
            ijson(item.owner) + ",\"count\":" + ijson(item.count) + ",\"price\":" + ijson(item.price) +
            ",\"flag\":" + ijson(item.flag) + ",\"blob\":" + ijson(item.blob) + ",\"created\":" +
            ijson(item.created) + ",\"ttl\":" + ijson(item.ttl) + ",\"note\":" + ijson(item.note) +
            ",\"labels\":" + ijson(item.labels) + ",\"range\":" + ijson(item.range) + ",\"big\":" +
            ijson(item.big) + ",\"small\":" + ijson(item.small) + "}}}");
}

TEST_CASE("graphql: objects, lists, tuples, unions, optionals", "[graphql][execute]")
{
   CHECK(run("{ item(id: 1003) { tags { label weight } labels range pair { _0 _1 } best { label weight } "
             "detail { __typename ... on Tag { label } ... on Extra { text codes } } } }") ==
         R"({"data":{"item":{"tags":[{"label":"tag0","weight":0},{"label":"tag1","weight":10},)"
         R"({"label":"tag2","weight":20}],"labels":["x","y\tz"],"range":[3,6],"pair":{"_0":3,"_1":"p3"},)"
         R"("best":{"label":"best","weight":99},"detail":{"__typename":"Extra","text":"extra","codes":[1,-2,3]}}}})");
   CHECK(run("{ item(id: 5) { id } nothing grid search { __typename ... on Tag { label } ... on Extra { codes } } }") ==
         R"({"data":{"item":null,"nothing":null,"grid":[[1,2],[],[3]],"search":[{"__typename":"Tag","label":"a"},)"
         R"({"__typename":"Extra","codes":[1,2]},{"__typename":"Tag","label":"b"}]}})");
   // Fields appear in the order of the selection, merged at first use.
   CHECK(run("{ item(id: 1000) { small id ... on Item { count small } id } }") ==
         R"({"data":{"item":{"small":0,"id":1000,"count":-2}}})");
   CHECK(run("{ __typename node { __typename id key renamed old heavy } }") ==
         R"({"data":{"__typename":"Query","node":{"__typename":"Node","id":"7","key":"k","renamed":3,"old":1,"heavy":9}}})");
}

TEST_CASE("graphql: aliases, fragments, type conditions", "[graphql][execute]")
{
   CHECK(run("{ first: item(id: 1000) { ...Brief } second: item(id: 1001) { ...Brief extra: count } } "
             "fragment Brief on Item { n: name ... on Item { c: count } }") ==
         R"({"data":{"first":{"n":"item \"0\"\n","c":-2},"second":{"n":"item \"1\"\n","c":-1,"extra":-1}}})");
   CHECK(run("{ search { ...T ...E } } fragment T on Tag { kind: __typename w: weight } "
             "fragment E on Extra { kind: __typename t: text }") ==
         R"({"data":{"search":[{"kind":"Tag","w":1},{"kind":"Extra","t":"e"},{"kind":"Tag","w":2}]}})");
   // A fragment on the union itself applies to every member.
   CHECK(run("{ search { ... on Tag_or_Extra { __typename } ... on Extra { codes } } }") ==
         R"({"data":{"search":[{"__typename":"Tag"},{"__typename":"Extra","codes":[1,2]},{"__typename":"Tag"}]}})");
   CHECK(run("{ a: add(a: 1, b: 2) b: add(a: 3, b: 4) add(a: 5, b: 6) }") == R"({"data":{"a":3,"b":7,"add":11}})");
}

TEST_CASE("graphql: @skip and @include", "[graphql][execute]")
{
   CHECK(run("{ a: add(a: 1, b: 1) @skip(if: true) b: add(a: 2, b: 2) @skip(if: false) "
             "c: add(a: 3, b: 3) @include(if: true) d: add(a: 4, b: 4) @include(if: false) "
             "e: add(a: 5, b: 5) @skip(if: false) @include(if: true) f: add(a: 6, b: 6) @skip(if: true) @include(if: true) }") ==
         R"({"data":{"b":4,"c":6,"e":10}})");
   const std::string_view q =
      "query ($s: Boolean!, $i: Boolean = true) { nothing @skip(if: $s) ... @include(if: $i) { nan node { id } } "
      "...F @skip(if: $s) node { key @include(if: $i) } } fragment F on Query { echo(text: \"f\") }";
   CHECK(run(q, R"({"s": false})") == R"({"data":{"nothing":null,"nan":0,"node":{"id":"7","key":"k"},"echo":"f"}})");
   CHECK(run(q, R"({"s": true})") == R"({"data":{"nan":0,"node":{"id":"7","key":"k"}}})");
   CHECK(run(q, R"({"s": true, "i": false})") == R"({"data":{"node":{}}})");
   // `node` is first selected inside the excluded fragment: its place is
   // that of the selection that is included.
   CHECK(run(q, R"({"s": false, "i": false})") == R"({"data":{"nothing":null,"echo":"f","node":{}}})");
   const std::string_view order = "query ($a: Boolean!, $b: Boolean!) { x: add(a: 1, b: 1) @include(if: $a) nothing "
                                  "x: add(a: 1, b: 1) @include(if: $b) nan x: add(a: 1, b: 1) y: nothing }";
   CHECK(run(order, R"({"a": true, "b": true})") == R"({"data":{"x":2,"nothing":null,"nan":0,"y":null}})");
   CHECK(run(order, R"({"a": false, "b": true})") == R"({"data":{"nothing":null,"x":2,"nan":0,"y":null}})");
   CHECK(run(order, R"({"a": false, "b": false})") == R"({"data":{"nothing":null,"nan":0,"x":2,"y":null}})");
   // A field selected twice is included when either selection is.
   const std::string_view twice = "query ($a: Boolean!, $b: Boolean!) { node @include(if: $a) { id } node @include(if: $b) { key } }";
   CHECK(run(twice, R"({"a": true, "b": true})") == R"({"data":{"node":{"id":"7","key":"k"}}})");
   CHECK(run(twice, R"({"a": true, "b": false})") == R"({"data":{"node":{"id":"7"}}})");
   CHECK(run(twice, R"({"a": false, "b": true})") == R"({"data":{"node":{"key":"k"}}})");
   CHECK(run(twice, R"({"a": false, "b": false})") == R"({"data":{}})");
}

TEST_CASE("graphql: arguments", "[graphql][execute]")
{
   CHECK(run(R"({ echo(text: "hi") add(a: -2, b: 5) tiny(v: -128) half(value: 5) maybe m2: maybe(text: null) m3: maybe(text: "x") })") ==
         R"({"data":{"echo":"hi","add":3,"tiny":-128,"half":2.5,"maybe":null,"m2":null,"m3":"x"}})");
   CHECK(run(R"({ flip(status: active) f2: flip(status: closed) owner(name: "acct-41") big(value: 7) b2: big(value: "18446744073709551615") })") ==
         R"({"data":{"flip":"frozen","f2":"active","owner":"acct-42","big":7,"b2":"18446744073709551615"}})");
   CHECK(run(R"({ bytes(data: "00ff10") later(at: "2026-11-22T10:15:30Z") l2: later(at: "2026-11-22T12:15:30+02:00") })") ==
         R"({"data":{"bytes":"00FF10","later":"2026-11-22T10:16:30Z","l2":"2026-11-22T10:16:30Z"}})");
   CHECK(run(R"({ ints(values: [3, 2, 1]) one: ints(values: 7) none: ints(values: []) total(grid: [[1, 2], [3]]) t2: total(grid: 5) })") ==
         R"({"data":{"ints":[3,2,1],"one":[7],"none":[],"total":6,"t2":5}})");
   CHECK(run(R"({ describe(filter: {names: ["a", "b"], status: frozen, minCount: 2, range: {lo: 1, hi: 9}})
                  d2: describe(filter: {names: "solo"}) d3: describe(filter: {names: [], status: null}) })") ==
         R"({"data":{"describe":"status=frozen min=2 names=a,b, range=1..9","d2":"status=- min=- names=solo, range=-","d3":"status=- min=- names= range=-"}})");
   CHECK(run("{ item(id: 1001) { scaled(by: 4) title } itemsView { scaled(by: -1) } }")
            .starts_with(R"({"data":{"item":{"scaled":-4,"title":"item \"1\"\n#1001"},"itemsView":[{"scaled":2},{"scaled":1},)"));
}

TEST_CASE("graphql: variables", "[graphql][execute]")
{
   const std::string_view q =
      "query Q($a: Int!, $b: Int = 10, $t: String, $l: [Int!]!, $f: FilterInput!, $s: Status = closed, "
      "$n: AccountName!, $u: UInt64!, $at: DateTimeSeconds!, $x: Bytes!) { add(a: $a, b: $b) maybe(text: $t) "
      "ints(values: $l) i2: ints(values: [$a, $b]) describe(filter: $f) d2: describe(filter: {names: [], status: $s, "
      "range: {lo: $a, hi: $b}}) owner(name: $n) big(value: $u) later(at: $at) bytes(data: $x) }";
   CHECK(run(q, R"({"a": 1, "l": [4, 5], "f": {"names": ["v"], "status": "active", "minCount": 3},
                    "n": "acct-9", "u": "18446744073709551615", "at": "2026-01-01T00:00:00Z", "x": "abcd",
                    "unused": {"anything": [1, 2]}})") ==
         R"({"data":{"add":11,"maybe":null,"ints":[4,5],"i2":[1,10],"describe":"status=active min=3 names=v, range=-",)"
         R"("d2":"status=closed min=- names= range=1..10","owner":"acct-10","big":"18446744073709551615",)"
         R"("later":"2026-01-01T00:01:00Z","bytes":"ABCD"}})");
   CHECK(run(q, R"({"a": 1, "b": 2, "t": "text é \"q\"", "l": [], "f": {"names": []}, "s": null, "n": "acct-0",
                    "u": 5, "at": "2026-01-01T00:00:00Z", "x": ""})") ==
         R"({"data":{"add":3,"maybe":"text é \"q\"","ints":[],"i2":[1,2],"describe":"status=- min=- names= range=-",)"
         R"("d2":"status=- min=- names= range=1..2","owner":"acct-1","big":5,"later":"2026-01-01T00:01:00Z","bytes":""}})");

   // Coercion failures are request errors located at the variable.
   auto fails = [&](std::string_view variables, std::string_view message) {
      const std::string out = run(q, variables);
      INFO(out);
      CHECK(out.find("\"data\"") == std::string::npos);
      CHECK(out.find(message) != std::string::npos);
   };
   const std::string rest = R"("l": [], "f": {"names": []}, "n": "acct-0", "u": 1, "at": "2026-01-01T00:00:00Z", "x": "")";
   fails("{" + rest + "}", R"({"message":"Variable \"$a\" of required type \"Int!\" was not provided.","locations":[{"line":1,"column":9}]})");
   fails(R"({"a": null, )" + rest + "}", R"(Variable \"$a\" got invalid value; Expected value of type \"Int!\", found null.)");
   fails(R"({"a": "1", )" + rest + "}", R"(Variable \"$a\" got invalid value; Expected value of type \"Int\", found \"1\".)");
   fails(R"({"a": 1.5, )" + rest + "}", R"(Variable \"$a\" got invalid value; Expected value of type \"Int\", found 1.5.)");
   fails(R"({"a": 4294967296, )" + rest + "}", R"(Variable \"$a\" got invalid value; Expected value of type \"Int\", found 4294967296.)");
   fails(R"({"a": 1, "s": "nope", )" + rest + "}", R"(Variable \"$s\" got invalid value; Value \"nope\" does not exist in \"Status\" enum.)");
   fails(R"({"a": 1, "s": 1, )" + rest + "}", R"(Variable \"$s\" got invalid value; Expected value of type \"Status\", found 1.)");
   fails(R"({"a": 1, "l": [1, null], "f": {"names": []}, "n": "acct-0", "u": 1, "at": "2026-01-01T00:00:00Z", "x": ""})",
         R"(Variable \"$l\" got invalid value; Expected value of type \"Int!\", found null.)");
   fails(R"({"a": 1, "l": [], "f": {"names": [], "bogus": 1}, "n": "acct-0", "u": 1, "at": "2026-01-01T00:00:00Z", "x": ""})",
         R"(Variable \"$f\" got invalid value; Field \"bogus\" is not defined by type \"FilterInput\".)");
   fails(R"({"a": 1, "l": [], "f": {}, "n": "acct-0", "u": 1, "at": "2026-01-01T00:00:00Z", "x": ""})",
         R"(Variable \"$f\" got invalid value; Field \"FilterInput.names\" of required type \"[String!]!\" was not provided.)");
   fails(R"({"a": 1, "l": [], "f": {"names": []}, "n": "bob", "u": 1, "at": "2026-01-01T00:00:00Z", "x": ""})",
         R"(Variable \"$n\" got invalid value; Expected value of type \"AccountName\", found \"bob\".)");
   fails(R"({"a": 1, "l": [], "f": {"names": []}, "n": "acct-0", "u": -1, "at": "2026-01-01T00:00:00Z", "x": ""})",
         R"(Variable \"$u\" got invalid value; Expected value of type \"UInt64\", found -1.)");
   fails(R"({"a": 1, )" + rest, "Variables are not a valid JSON object.");
   fails(R"([1, 2])", "Variables are not a valid JSON object.");
   fails(R"({"a": 1, "t": "\ud800", )" + rest + "}", "Variables are not a valid JSON object.");

   // A value in range for Int but not for the C++ parameter is a field error.
   CHECK(run("query ($v: Int!) { tiny(v: $v) nothing }", R"({"v": 300})") ==
         R"({"data":null,"errors":[{"message":"Argument \"v\" has an invalid value.","locations":[{"line":1,"column":20}],"path":["tiny"]}]})");
   // An absent nullable variable is an absent argument; null is null.
   CHECK(run("query ($t: String) { maybe(text: $t) }") == R"({"data":{"maybe":null}})");
   CHECK(run("query ($t: String) { maybe(text: $t) }", "null") == R"({"data":{"maybe":null}})");
   CHECK(run("query ($t: String = \"d\") { maybe(text: $t) }") == R"({"data":{"maybe":"d"}})");
   CHECK(run("query ($t: String = \"d\") { maybe(text: $t) }", R"({"t": null})") == R"({"data":{"maybe":null}})");
   // A nullable variable with a default may be used where non-null is
   // expected; an explicit null then fails at the field.
   CHECK(run("query ($a: Int = 3) { add(a: $a, b: 1) }") == R"({"data":{"add":4}})");
   CHECK(run("query ($a: Int = 3) { add(a: $a, b: 1) }", R"({"a": null})").starts_with(R"({"data":null,"errors":[{"message":"Argument \"a\" has an invalid value.")"));
}

TEST_CASE("graphql: operation selection", "[graphql][execute]")
{
   const Query            q   = make_query();
   const std::string_view doc = "query A { add(a: 1, b: 1) } query B { add(a: 2, b: 2) }";
   CHECK(run(q, doc, {.operation_name = "A"}) == R"({"data":{"add":2}})");
   CHECK(run(q, doc, {.operation_name = "B"}) == R"({"data":{"add":4}})");
   CHECK(run(q, doc) == R"({"errors":[{"message":"Must provide operation name if query contains multiple operations."}]})");
   CHECK(run(q, doc, {.operation_name = "C"}) == R"({"errors":[{"message":"Unknown operation named \"C\"."}]})");
   CHECK(run(q, "query Only { nothing }") == R"({"data":{"nothing":null}})");
   CHECK(run(q, "query Only { nothing }", {.operation_name = "Only"}) == R"({"data":{"nothing":null}})");
   CHECK(run(q, "query Only { nothing }", {.operation_name = "Other"}) == R"({"errors":[{"message":"Unknown operation named \"Other\"."}]})");
}

TEST_CASE("graphql: errors and null propagation", "[graphql][execute]")
{
   // A failed nullable field is null; the rest of the response stands.
   CHECK(run("{ risky(ok: false) fine: risky(ok: true) nothing }") ==
         R"({"data":{"risky":null,"fine":5,"nothing":null},"errors":[{"message":"risky failed","locations":[{"line":1,"column":3}],"path":["risky"]}]})");
   CHECK(run("{ tagOrError(ok: false) { label } t2: tagOrError(ok: true) { label } }") ==
         R"({"data":{"tagOrError":null,"t2":{"label":"t"}},"errors":[{"message":"no tag","locations":[{"line":1,"column":3}],"path":["tagOrError"]}]})");
   // A non-null field that cannot be written nulls its nearest nullable
   // parent: `nested` here.
   CHECK(run("{ nested { ok bad } nothing }") ==
         R"({"data":{"nested":null,"nothing":null},"errors":[{"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":1,"column":15}],"path":["nested","bad"]}]})");
   CHECK(run("{ nested { ok } }") == R"({"data":{"nested":{"ok":1}}})");
   // ... a list element ...
   CHECK(run("{ nullableList { ok bad } }") ==
         R"({"data":{"nullableList":[{"ok":1,"bad":"fine"},null]},"errors":[{"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":1,"column":21}],"path":["nullableList",1,"bad"]}]})");
   // ... or, with no nullable parent, the whole response.
   CHECK(run("{ nothing strict { ok bad } }") ==
         R"({"data":null,"errors":[{"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":1,"column":23}],"path":["strict","bad"]}]})");
   CHECK(run("{ nestedList { bad } }") ==
         R"({"data":null,"errors":[{"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":1,"column":16}],"path":["nestedList",1,"bad"]}]})");
   CHECK(run("{ badText }") ==
         R"({"data":null,"errors":[{"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":1,"column":3}],"path":["badText"]}]})");
   CHECK(run("{ badEnum }") ==
         R"({"data":null,"errors":[{"message":"Enum \"Status\" has no name for this value.","locations":[{"line":1,"column":3}],"path":["badEnum"]}]})");
   // Every failure is reported, with the alias in the path.
   CHECK(run("{\n  a: risky(ok: false)\n  b: nested { bad }\n  c: nullableList { bad }\n}") ==
         R"({"data":{"a":null,"b":null,"c":[{"bad":"fine"},null]},"errors":[)"
         R"({"message":"risky failed","locations":[{"line":2,"column":3}],"path":["a"]},)"
         R"({"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":3,"column":15}],"path":["b","bad"]},)"
         R"({"message":"Cannot represent value: string is not valid UTF-8","locations":[{"line":4,"column":21}],"path":["c",1,"bad"]}]})");
   // A non-finite float has no JSON text.
   Query q = make_query();
   q.nan   = std::numeric_limits<float>::quiet_NaN();
   CHECK(run(q, "{ nan }") ==
         R"({"data":null,"errors":[{"message":"Cannot represent value: non-finite float has no JSON text","locations":[{"line":1,"column":3}],"path":["nan"]}]})");
}

TEST_CASE("graphql: request context", "[graphql][execute]")
{
   const Query q = make_query();
   Session     session{"alice"};
   gql::request req;
   req.user(session);
   CHECK(run(q, "{ viewer { name calls } v2: viewer { calls } greet(greeting: \"hi\") }", req) ==
         R"({"data":{"viewer":{"name":"alice","calls":1},"v2":{"calls":2},"greet":"hi, alice"}})");
   CHECK(session.calls == 2);
   // No context, or one of another type: user<Session>() is null.
   CHECK(run(q, "{ viewer { name } greet(greeting: \"yo\") }") == R"({"data":{"viewer":{"name":"anonymous"},"greet":"yo, nobody"}})");
   Other other;
   gql::request req2;
   req2.user(other);
   CHECK(run(q, "{ viewer { name } }", req2) == R"({"data":{"viewer":{"name":"anonymous"}}})");
   // The context parameter is not a GraphQL argument.
   rejects("{ viewer(~ctx: 1) { name } }", "Unknown argument \"ctx\" on field \"Query.viewer\".");
}

TEST_CASE("graphql: mutations run in order on the mutation root", "[graphql][execute]")
{
   const Query q = make_query();
   Mutation    m;
   std::string out;
   CHECK(api::run(q, m, "mutation { a: bump(by: 2) b: bump(by: 3) value }", out) == 0);
   CHECK(out == R"({"data":{"a":2,"b":5,"value":5}})");
   CHECK(m.counter == 5);
   out.clear();
   CHECK(api::run(q, m, "mutation M { reset value failing after: value } query Q { nothing }", out, {.operation_name = "M"}) == 1);
   CHECK(out == R"({"data":{"reset":true,"value":0,"failing":null,"after":1},"errors":[{"message":"mutation failed","locations":[{"line":1,"column":26}],"path":["failing"]}]})");
   out.clear();
   CHECK(api::run(q, m, "mutation M { reset } query Q { nothing }", out, {.operation_name = "Q"}) == 0);
   CHECK(out == R"({"data":{"nothing":null}})");
   // Without the mutation root a mutation cannot run.
   out.clear();
   CHECK(api::run(q, "mutation { reset }", out) == 1);
   CHECK(out == R"({"data":null,"errors":[{"message":"A mutation needs the mutation root."}]})");
   CHECK(m.counter == 1);
   // A schema without a mutation type.
   gql::document           plan;
   std::vector<gql::error> errors;
   CHECK_FALSE(gql::schema<Query>::compile("mutation { reset }", plan, errors));
   REQUIRE(errors.size() == 1);
   CHECK(errors[0].message == "Schema is not configured for mutations.");
}

// ── Sources ─────────────────────────────────────────────────────────────

TEST_CASE("graphql: values and PSSZ views give byte-identical responses", "[graphql][view]")
{
   const Query q = make_query(9);
   const std::string_view selections[] = {
      "id",
      "name note",
      "id name owner status count price flag blob created ttl note big small rev",
      "tags { label weight } labels range pair { _0 _1 } best { weight label }",
      "detail { __typename ... on Tag { label weight } ... on Extra { text codes } }",
      "rev best { label } detail { ... on Extra { codes } } tags { weight } pair { _1 } range",
      "__typename title scaled(by: 2) x: id y: id",
      "id name owner status count price flag blob created ttl note tags { label weight } labels range "
      "pair { _0 _1 } detail { __typename ... on Tag { label weight } ... on Extra { text codes } } big small "
      "best { label weight } rev",
   };
   for (const std::string_view selection : selections)
   {
      INFO(selection);
      const std::string by_value = run(q, "{ r: items { " + std::string{selection} + " } }");
      const std::string by_view  = run(q, "{ r: itemsView { " + std::string{selection} + " } }");
      CHECK(by_value == by_view);
      CHECK(by_value.find("\"errors\"") == std::string::npos);
      // One row through an owning buffer, as a method would return a
      // database row it read.
      const std::string one_value  = run(q, "{ r: item(id: 1004) { " + std::string{selection} + " } }");
      const std::string one_buffer = run(q, "{ r: itemBuffer(at: 4) { " + std::string{selection} + " } }");
      CHECK(one_value == one_buffer);
   }
}

TEST_CASE("graphql: wide integers, fixed bytes and nested shapes, by value and by view", "[graphql][view]")
{
   const std::string_view selection =
      "u128 i128 u256 fixed maybe_big holes nested { label weight } two { label } maybe_list no_tag { label }";
   const std::string by_value = run("{ r: wide { " + std::string{selection} + " } }");
   CHECK(by_value ==
         R"({"data":{"r":{"u128":"1267650600228229401496703205383","i128":"-1237940039285380274899124224",)"
         R"("u256":"6277101735386680763835789423207666416102355444464034525241","fixed":"DEADBEEF",)"
         R"("maybe_big":"9007199254740992","holes":["a",null,""],"nested":[[{"label":"n","weight":1}],[],)"
         R"([{"label":"m","weight":2},{"label":"o","weight":3}]],"two":[{"label":"first"},{"label":"second"}],)"
         R"("maybe_list":[],"no_tag":null}}})");
   CHECK(run("{ r: wideView { " + std::string{selection} + " } }") == by_value);
   const Query q = make_query(1);
   CHECK(by_value.find("\"u128\":" + psio::encode(psio::ijson{}, q.wide.u128)) != std::string::npos);
   CHECK(by_value.find("\"u256\":" + psio::encode(psio::ijson{}, q.wide.u256)) != std::string::npos);
}

TEST_CASE("graphql: one GraphQL type through several sources", "[graphql][view]")
{
   // References, pointers, views: all `Item`; connections over views and
   // over references: one `ItemConnection`.
   const Query q = make_query(4);
   const std::string_view sel = "id name tags { label } detail { __typename } rev";
   const std::string      ref = run(q, "{ r: items { " + std::string{sel} + " } }");
   CHECK(run(q, "{ r: itemRefs { " + std::string{sel} + " } }") == ref);
   CHECK(run(q, "{ firstItem { id } }") == R"({"data":{"firstItem":{"id":1000}}})");
   CHECK(run(Query{}, "{ firstItem { id } }") == R"({"data":{"firstItem":null}})");
   CHECK(api::sdl().find("  itemPages(first: Int, after: String, last: Int, before: String): ItemConnection\n") !=
         std::string::npos);
   CHECK(api::sdl().find("  itemRefs: [Item!]!\n  firstItem: Item\n") != std::string::npos);
   const std::string_view page = "edges { node { id title } } pageInfo { hasNextPage hasPreviousPage }";
   CHECK(run(q, "{ itemPages(first: 2) { " + std::string{page} + " } }") ==
         R"({"data":{"itemPages":{"edges":[{"node":{"id":1000,"title":"item \"0\"\n#1000"}},{"node":{"id":1001,"title":"item \"1\"\n#1001"}}],"pageInfo":{"hasNextPage":true,"hasPreviousPage":false}}}})");
   CHECK(run(q, "{ itemPages(last: 1) { edges { cursor node { id } } pageInfo { hasNextPage hasPreviousPage } } }") ==
         R"({"data":{"itemPages":{"edges":[{"cursor":"00000003","node":{"id":1003}}],"pageInfo":{"hasNextPage":false,"hasPreviousPage":true}}}})");
   // A fragment on the shared type applies to either connection.
   CHECK(run(q, "{ a: itemPages(first: 1) { ...P } b: accounts(first: 1) { ...P } } "
                "fragment P on ItemConnection { edges { ...E } } fragment E on ItemEdge { node { ... on Item { id } } }") ==
         R"({"data":{"a":{"edges":[{"node":{"id":1000}}]},"b":{"edges":[{"node":{"id":1000}}]}}})");
}

TEST_CASE("graphql: a view reads only the selected fields", "[graphql][view]")
{
   // The encoded rows hold a name that is not UTF-8 (PSSZ stores raw
   // bytes). Queries that do not select it never look at it.
   Query q = make_query(3);
   for (Item& i : q.items)
      i.name = "\xff\xfe raw";
   q.encoded = psio::encode(psio::pssz{}, q.items);
   CHECK(run(q, "{ itemsView { id count labels } }") ==
         R"({"data":{"itemsView":[{"id":1000,"count":-2,"labels":["x","y\tz"]},{"id":1001,"count":-1,"labels":["x","y\tz"]},{"id":"18446744073709551615","count":0,"labels":["x","y\tz"]}]}})");
   CHECK(run(q, "{ itemsView { id name } }").starts_with(R"({"data":null,"errors":[{"message":"Cannot represent value: string is not valid UTF-8")"));
}

TEST_CASE("graphql: record evolution through a view", "[graphql][view]")
{
   // The row was written before `rev` (a trailing optional) existed.
   CHECK(run("{ oldItem { id name owner status labels best { label } detail { __typename } rev title } }") ==
         R"({"data":{"oldItem":{"id":77,"name":"old","owner":"acct-4","status":"active","labels":["l"],"best":{"label":"b"},"detail":{"__typename":"Tag"},"rev":null,"title":"old#77"}}})");
}

TEST_CASE("graphql: lists are produced lazily", "[graphql][execute]")
{
   const Query q = make_query();
   CHECK(run(q, "{ squares(first: 5) }") == R"({"data":{"squares":[0,1,4,9,16]}})");
   CHECK(q.produced == 5);
   CHECK(run(q, "{ squares(first: 0) }") == R"({"data":{"squares":[]}})");
   CHECK(q.produced == 5);
}

// ── Connections ─────────────────────────────────────────────────────────

TEST_CASE("graphql: Relay connections", "[graphql][connection]")
{
   const Query q = make_query(11);
   // Keys are psio::key(owner, id): rows come out ordered by owner, then id.
   std::vector<std::string> expected;
   for (const auto& [key, row] : q.index.rows)
      expected.push_back(std::to_string(psio::decode<Item>(psio::pssz{}, std::span<const char>{row}).count));

   const std::string_view page = "query ($first: Int, $after: String, $last: Int, $before: String) { "
                                 "accounts(first: $first, after: $after, last: $last, before: $before) { "
                                 "edges { cursor node { count } } pageInfo { hasNextPage hasPreviousPage startCursor endCursor } } }";
   // Forward, three at a time.
   std::vector<std::string> seen;
   std::string              after = "null";
   std::size_t              pages = 0;
   while (true)
   {
      const J r = parse_json(run(q, page, gql::request{.variables = R"({"first": 3, "after": )" + after + "}"}));
      const J& c = r["data"]["accounts"];
      REQUIRE(c.kind == psio::json_kind::object);
      for (const J& e : c["edges"].items)
         seen.push_back(e["node"]["count"].text);
      CHECK(c["pageInfo"]["hasPreviousPage"].flag == (pages != 0));
      CHECK(c["pageInfo"]["startCursor"].text == c["edges"].items.front()["cursor"].text);
      CHECK(c["pageInfo"]["endCursor"].text == c["edges"].items.back()["cursor"].text);
      ++pages;
      if (!c["pageInfo"]["hasNextPage"].flag)
         break;
      after = "\"" + c["pageInfo"]["endCursor"].text + "\"";
   }
   CHECK(pages == 4);
   CHECK(seen == expected);

   // Backward, four at a time (the page-size cap of this field).
   seen.clear();
   std::string before = "null";
   pages              = 0;
   while (true)
   {
      const J r = parse_json(run(q, page, gql::request{.variables = R"({"last": 50, "before": )" + before + "}"}));
      const J& c = r["data"]["accounts"];
      REQUIRE(c["edges"].items.size() <= 4);
      for (std::size_t i = c["edges"].items.size(); i-- > 0;)
         seen.push_back(c["edges"].items[i]["node"]["count"].text);
      CHECK(c["pageInfo"]["hasNextPage"].flag == (pages != 0));
      ++pages;
      if (!c["pageInfo"]["hasPreviousPage"].flag)
         break;
      before = "\"" + c["pageInfo"]["startCursor"].text + "\"";
   }
   CHECK(pages == 3);
   std::reverse(seen.begin(), seen.end());
   CHECK(seen == expected);

   // A window: after and before together.
   const J all = parse_json(run(q, "{ accountsOf(owner: \"acct-0\", first: 100) { edges { cursor node { id owner } } } }"));
   const auto& edges = all["data"]["accountsOf"]["edges"].items;
   REQUIRE(edges.size() == 4);  // owners are n % 3: n = 0, 3, 6, 9
   for (const J& e : edges)
      CHECK(e["node"]["owner"].text == "acct-0");
   CHECK(run(q, page, gql::request{.variables = R"({"first": 10, "after": ")" + edges[0]["cursor"].text + R"(", "before": ")" +
                                                edges[3]["cursor"].text + "\"}"})
            .find(R"("pageInfo":{"hasNextPage":false,"hasPreviousPage":true,)") != std::string::npos);
   // A key prefix restricts the source; an owner without rows is empty.
   CHECK(run(q, "{ accountsOf(owner: \"acct-7\") { edges { cursor } pageInfo { hasNextPage hasPreviousPage startCursor endCursor } } }") ==
         R"({"data":{"accountsOf":{"edges":[],"pageInfo":{"hasNextPage":false,"hasPreviousPage":false,"startCursor":null,"endCursor":null}}}})");
   CHECK(run(q, "{ accounts(first: 0) { edges { cursor } pageInfo { hasNextPage } } }") ==
         R"({"data":{"accounts":{"edges":[],"pageInfo":{"hasNextPage":true}}}})");
   // Invalid arguments are field errors.
   CHECK(run(q, "{ accounts(first: 1, last: 1) { edges { cursor } } }") ==
         R"({"data":{"accounts":null},"errors":[{"message":"Arguments \"first\" and \"last\" cannot both be given.","locations":[{"line":1,"column":3}],"path":["accounts"]}]})");
   CHECK(run(q, "{ accounts(first: -1) { edges { cursor } } }").find("must not be negative") != std::string::npos);
   CHECK(run(q, "{ accounts(after: \"xyz\") { edges { cursor } } }").find("Argument \\\"after\\\" is not a cursor.") != std::string::npos);

   // One page reads one page: two rows, the look-ahead, nothing else, however
   // large the index.
   const Query big = make_query(200);
   big.row_reads   = 0;
   CHECK(run(big, "{ accounts(first: 2) { edges { node { id } } pageInfo { hasNextPage } } }").ends_with(R"("pageInfo":{"hasNextPage":true}}}})"));
   CHECK(big.row_reads == 3);
}

// ── Introspection ───────────────────────────────────────────────────────

TEST_CASE("graphql: SDL", "[graphql][introspection]")
{
   const std::string sdl = api::sdl();
   CHECK(sdl.starts_with("type Query {\n  items: [Item!]!\n  node: Node!\n  wide: Wide!\n  nothing: Int\n  nan: Float!\n"
                         "  itemsView: [Item!]!\n  item(id: UInt64!): Item\n"));
   for (const std::string_view part : {
           "  describe(filter: FilterInput!): String!\n",
           "  total(grid: [[Int!]!]!): Int!\n",
           "  bytes(data: Bytes!): Bytes!\n",
           "  nullableList: [Nested]!\n",
           "  viewer: Viewer!\n",
           "  accounts(first: Int, after: String, last: Int, before: String): ItemConnection\n",
           "type Mutation {\n  bump(by: Int!): Int!\n  reset: Boolean!\n  value: Int!\n  failing: Int\n}\n",
           "\"An annotated node.\"\ntype Node {\n  id: ID!\n  \"The key.\"\n  key: ID!\n  renamed: Int!\n"
           "  old: Int! @deprecated(reason: \"use renamed\")\n  heavy: Int!\n}\n",
           "input FilterInput {\n  status: Status\n  minCount: Int\n  names: [String!]!\n  range: RangeInput\n}\n",
           "enum Status {\n  active\n  frozen\n  closed\n}\n",
           "union Tag_or_Extra = Tag | Extra\n",
           "type Tuple_Int_String {\n  _0: Int!\n  _1: String!\n}\n",
           "type ItemConnection {\n  edges: [ItemEdge!]!\n  pageInfo: PageInfo!\n}\n",
           "type ItemEdge {\n  node: Item!\n  cursor: String!\n}\n",
           "type PageInfo {\n  hasNextPage: Boolean!\n  hasPreviousPage: Boolean!\n  startCursor: String\n  endCursor: String\n}",
           "scalar AccountName\n", "scalar UInt64\n", "scalar Int64\n", "scalar Bytes\n", "scalar DateTimeSeconds\n",
           "scalar Milliseconds\n",
           "  range: [Int!]!\n  pair: Tuple_Int_String!\n  detail: Tag_or_Extra!\n  big: Int64!\n  small: Int!\n"
           "  best: Tag\n  rev: Int\n  title: String!\n  scaled(by: Int!): Int!\n}\n",
        })
   {
      INFO(part);
      CHECK(sdl.find(part) != std::string::npos);
   }
   CHECK(sdl.find("secret") == std::string::npos);
   CHECK(sdl.find("__") == std::string::npos);
   CHECK(sdl.find("encoded") == std::string::npos);
}

TEST_CASE("graphql: introspection round trip", "[graphql][introspection]")
{
   // The introspection query every client sends; its cost is well inside
   // the default complexity limit.
   const std::string response = run(introspection_query);
   REQUIRE(response.find("\"errors\"") == std::string::npos);
   const J schema = parse_json(response)["data"]["__schema"];
   REQUIRE(schema.kind == psio::json_kind::object);
   CHECK(sdl_from_introspection(schema) == api::sdl());

   // The introspection types themselves are listed, as are the built-ins.
   std::vector<std::string> names;
   for (const J& t : schema["types"].items)
      names.push_back(t["name"].text);
   for (const char* expected : {"Query", "Mutation", "Int", "Float", "String", "Boolean", "ID", "__Schema", "__Type",
                                "__TypeKind", "__Field", "__InputValue", "__EnumValue", "__Directive",
                                "__DirectiveLocation"})
   {
      INFO(expected);
      CHECK(std::count(names.begin(), names.end(), expected) == 1);
   }
   std::vector<std::string> sorted = names;
   std::sort(sorted.begin(), sorted.end());
   CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
   CHECK(schema["directives"].items.size() == 3);
   CHECK(schema["subscriptionType"].is_null());

   CHECK(run(R"({ __type(name: "Status") { kind name enumValues { name isDeprecated } fields { name } } missing: __type(name: "Nope") { name } })") ==
         R"({"data":{"__type":{"kind":"ENUM","name":"Status","enumValues":[{"name":"active","isDeprecated":false},{"name":"frozen","isDeprecated":false},{"name":"closed","isDeprecated":false}],"fields":null},"missing":null}})");
   // Deprecated fields are left out unless asked for.
   CHECK(run(R"({ __type(name: "Node") { description fields { name } all: fields(includeDeprecated: true) { name isDeprecated deprecationReason description } } })") ==
         R"({"data":{"__type":{"description":"An annotated node.","fields":[{"name":"id"},{"name":"key"},{"name":"renamed"},{"name":"heavy"}],)"
         R"("all":[{"name":"id","isDeprecated":false,"deprecationReason":null,"description":null},{"name":"key","isDeprecated":false,"deprecationReason":null,"description":"The key."},)"
         R"({"name":"renamed","isDeprecated":false,"deprecationReason":null,"description":null},{"name":"old","isDeprecated":true,"deprecationReason":"use renamed","description":null},)"
         R"({"name":"heavy","isDeprecated":false,"deprecationReason":null,"description":null}]}}})");
   CHECK(run(R"({ __type(name: "Query") { fields { name type { kind ofType { kind ofType { kind ofType { name } } } } } } })")
            .find(R"({"name":"items","type":{"kind":"NON_NULL","ofType":{"kind":"LIST","ofType":{"kind":"NON_NULL","ofType":{"name":"Item"}}}}})") !=
         std::string::npos);
   CHECK(run(R"({ __schema { queryType { name kind } mutationType { name } } __typename })") ==
         R"({"data":{"__schema":{"queryType":{"name":"Query","kind":"OBJECT"},"mutationType":{"name":"Mutation"}},"__typename":"Query"}})");

   // A schema built without introspection has no __schema / __type.
   using bare = gql::schema<Query, void, false>;
   gql::document           plan;
   std::vector<gql::error> errors;
   CHECK_FALSE(bare::compile("{ __schema { queryType { name } } }", plan, errors));
   REQUIRE(errors.size() == 1);
   CHECK(errors[0].message == "Cannot query field \"__schema\" on type \"Query\".");
   CHECK(bare::compile("{ __typename nothing }", plan, errors));
   CHECK(bare::sdl() == gql::schema<Query>::sdl());
}

// ── Plans ───────────────────────────────────────────────────────────────

TEST_CASE("graphql: a plan is reusable across roots, variables and requests", "[graphql][plan]")
{
   gql::document           plan;
   std::vector<gql::error> errors;
   {
      // The plan keeps nothing of the query text.
      std::string text = "query Q($id: UInt64!, $skip: Boolean = false) { item(id: $id) { name count @skip(if: $skip) "
                         "tags { label } } echo(text: \"lit\") } mutation M($by: Int!) { bump(by: $by) }";
      REQUIRE(api::compile(text, plan, errors));
      std::fill(text.begin(), text.end(), '#');
   }
   const Query a = make_query(3);
   Query       b = make_query(6);
   b.items[1].name = "changed";
   Mutation    m;
   auto exec = [&](const Query& q, std::string_view op, std::string_view vars) {
      std::string out;
      api::execute(plan, q, m, out, gql::request{.operation_name = op, .variables = vars});
      return out;
   };
   CHECK(exec(a, "Q", R"({"id": 1001})") == R"({"data":{"item":{"name":"item \"1\"\n","count":-1,"tags":[{"label":"tag0"}]},"echo":"lit"}})");
   CHECK(exec(b, "Q", R"({"id": 1001, "skip": true})") == R"({"data":{"item":{"name":"changed","tags":[{"label":"tag0"}]},"echo":"lit"}})");
   CHECK(exec(a, "Q", R"({"id": 1005})") == R"({"data":{"item":null,"echo":"lit"}})");
   CHECK(exec(b, "Q", R"({"id": 1005})").starts_with(R"({"data":{"item":{"name":"item \"5\"\n","count":3,)"));
   CHECK(exec(a, "M", R"({"by": 4})") == R"({"data":{"bump":4}})");
   CHECK(exec(a, "M", R"({"by": 4})") == R"({"data":{"bump":8}})");
   CHECK(exec(a, "Q", "{}").find("Variable \\\"$id\\\" of required type \\\"UInt64!\\\" was not provided.") != std::string::npos);
   // execute() appends.
   std::string out = "prefix:";
   api::execute(plan, a, m, out, gql::request{.operation_name = "M", .variables = R"({"by": 1})"});
   CHECK(out == R"(prefix:{"data":{"bump":9}})");
   // A plan belongs to the schema it was compiled for.
   out.clear();
   CHECK(gql::schema<Query>::execute(plan, a, out) == 1);
   CHECK(out == R"({"errors":[{"message":"Document was compiled for a different schema."}]})");
}

TEST_CASE("graphql: plan cache", "[graphql][plan]")
{
   gql::plan_cache         cache{2};
   std::vector<gql::error> errors;
   const gql::document*    a = cache.get(api::desc, "{ nothing }", errors);
   REQUIRE(a);
   CHECK(cache.get(api::desc, "{ nothing }", errors) == a);
   CHECK(cache.find("{ nothing }") == a);
   CHECK(cache.find("{ nan }") == nullptr);
   CHECK(cache.size() == 1);
   // Invalid documents are reported and not cached.
   CHECK(cache.get(api::desc, "{ nope }", errors) == nullptr);
   CHECK(errors.size() == 1);
   CHECK(cache.size() == 1);
   const gql::document* b = cache.get(api::desc, "{ nan }", errors);
   REQUIRE(b);
   CHECK(errors.empty());
   CHECK(cache.size() == 2);
   // The least recently used entry is replaced.
   CHECK(cache.find("{ nothing }") == a);
   REQUIRE(cache.get(api::desc, "{ __typename }", errors));
   CHECK(cache.size() == 2);
   CHECK(cache.find("{ nan }") == nullptr);
   CHECK(cache.find("{ nothing }") != nullptr);
   const Query q = make_query(1);
   std::string out;
   api::execute(*cache.find("{ __typename }"), q, out);
   CHECK(out == R"({"data":{"__typename":"Query"}})");
}

// ── Limits ──────────────────────────────────────────────────────────────

namespace
{
   std::string limited(std::string_view text, const gql::limits& lim, std::string_view variables = {})
   {
      static const Query q = make_query();
      return run(q, text, gql::request{.variables = variables}, lim);
   }
   std::string first_message(const std::string& response)
   {
      const J r = parse_json(response);
      return r["errors"].items.empty() ? std::string{} : r["errors"].items[0]["message"].text;
   }
}  // namespace

TEST_CASE("graphql: limits", "[graphql][limits]")
{
   gql::limits lim;

   lim                    = {};
   lim.max_document_bytes = 16;
   CHECK(limited("{ nothing }", lim) == R"({"data":{"nothing":null}})");
   CHECK(first_message(limited("{ nothing nothing }", lim)) == "Document is larger than the limit allows.");

   lim            = {};
   lim.max_tokens = 5;
   CHECK(limited("{ nothing nan nothing }", lim) == R"({"data":{"nothing":null,"nan":0}})");
   CHECK(first_message(limited("{ nothing nan nothing nan }", lim)) == "Document has more tokens than the limit allows.");

   lim           = {};
   lim.max_depth = 3;
   CHECK(limited("{ items { tags { label } } }", lim).starts_with(R"({"data":{"items":[{"tags":[]},)"));
   CHECK(first_message(limited("{ items { detail { ... on Tag { label } } } }", lim)) ==
         "Selection sets are nested deeper than the limit allows.");
   // Depth is counted after fragments are expanded.
   CHECK(first_message(limited("{ items { ...A } } fragment A on Item { best { ...B } } fragment B on Tag { ...C } "
                               "fragment C on Tag { ...D } fragment D on Tag { label }",
                               lim)) == "");
   CHECK(first_message(limited("{ items { ...A } } fragment A on Item { best { ...B } } fragment B on Tag { ...C } "
                               "fragment C on Tag { ...D } fragment D on Tag { ...E } fragment E on Tag { label }",
                               lim)) == "Fragments are nested deeper than the limit allows.");
   CHECK(first_message(limited("{ ...A } fragment A on Query { items { ...B } } fragment B on Item { best { ...C } } "
                               "fragment C on Tag { label }",
                               lim)) == "");
   lim.max_depth = 2;
   CHECK(first_message(limited("{ ...A } fragment A on Query { items { ...B } } fragment B on Item { best { ...C } } "
                               "fragment C on Tag { label }",
                               lim)) == "Query is nested deeper than the limit allows.");

   // Fields after fragment expansion: an alias bomb and a fragment bomb.
   lim            = {};
   lim.max_fields = 100;
   std::string aliases = "{";
   for (int i = 0; i < 101; ++i)
      aliases += " a" + std::to_string(i) + ": nothing";
   aliases += " }";
   CHECK(first_message(limited(aliases, lim)) == "Query selects more fields than the limit allows.");
   std::string bomb = "{ ...F0 }";
   for (int i = 0; i < 20; ++i)
      bomb += " fragment F" + std::to_string(i) + " on Query { ...F" + std::to_string(i + 1) + " ...F" +
              std::to_string(i + 1) + " }";
   bomb += " fragment F20 on Query { nothing }";
   CHECK(first_message(limited(bomb, lim)) == "Query selects more fields than the limit allows.");
   // The same bomb with nothing to select at the bottom still stops.
   std::string hollow = "{ search { ...F0 } }";
   for (int i = 0; i < 24; ++i)
      hollow += " fragment F" + std::to_string(i) + " on Tag { ...F" + std::to_string(i + 1) + " ...F" +
                std::to_string(i + 1) + " }";
   hollow += " fragment F24 on Tag { ... on Tag_or_Extra { ... on Extra { text } } }";
   CHECK(first_message(limited(hollow, lim)) == "Query selects more fields than the limit allows.");

   lim                 = {};
   lim.max_input_depth = 2;
   CHECK(limited("{ total(grid: [[1]]) }", lim) == R"({"data":{"total":1}})");
   CHECK(first_message(limited("{ total(grid: [[[1]]]) }", lim)) == "Input value is nested deeper than the limit allows.");
   CHECK(limited("query ($g: [[Int!]!]!) { total(grid: $g) }", lim, R"({"g": [[1, 2]]})") == R"({"data":{"total":3}})");
   CHECK(first_message(limited("query ($g: [[Int!]!]!) { total(grid: $g) }", lim, R"({"g": [[[1]]]})")) ==
         "Variables are not a valid JSON object.");

   lim                     = {};
   lim.max_variables_bytes = 8;
   CHECK(first_message(limited("query ($t: String) { maybe(text: $t) }", lim, R"({"t": "long"})")) ==
         "Variables are larger than the limit allows.");

   // Complexity: 1 per field; the selection below a list is multiplied by
   // its page size (default_list_size without a first/last argument).
   lim                = {};
   lim.max_complexity = 21;  // items (1) + 10 * (id + name)
   CHECK(first_message(limited("{ items { id name } }", lim)) == "");
   lim.max_complexity = 20;
   CHECK(first_message(limited("{ items { id name } }", lim)) == "Query complexity exceeds the limit of 20.");
   lim.default_list_size = 2;  // 1 + 2 * 2
   CHECK(first_message(limited("{ items { id name } }", lim)) == "");
   // accounts (1) + first * (edges (1) + 10 * (node (1) + id (1)))
   lim                = {};
   lim.max_complexity = 1 + 3 * 21;
   CHECK(first_message(limited("{ accounts(first: 3) { edges { node { id } } } }", lim)) == "");
   CHECK(first_message(limited("{ accounts(first: 4) { edges { node { id } } } }", lim)) == "Query complexity exceeds the limit of 64.");
   // ... also when the page size is a variable (checked per request).
   CHECK(first_message(limited("query ($n: Int) { accounts(first: $n) { edges { node { id } } } }", lim, R"({"n": 3})")) == "");
   CHECK(first_message(limited("query ($n: Int) { accounts(last: $n) { edges { node { id } } } }", lim, R"({"n": 4})")) ==
         "Query complexity exceeds the limit of 64.");
   // A list of scalars costs its length; a huge page is refused up front.
   lim = {};
   CHECK(first_message(limited("{ squares(first: 2147483647) }", lim)) == "Query complexity exceeds the limit of 100000.");
   CHECK(first_message(limited("{ squares(first: 99999) }", lim)) == "");
   // psio::graphql::cost on a member.
   lim.max_complexity = 501;  // node (1) + heavy (500)
   CHECK(first_message(limited("{ node { heavy } }", lim)) == "");
   lim.max_complexity = 500;
   CHECK(first_message(limited("{ node { heavy } }", lim)) == "Query complexity exceeds the limit of 500.");
   // The standard introspection query fits the default.
   lim = {};
   CHECK(first_message(limited(introspection_query, lim)) == "");

   // Response limits stop a running request.
   lim                     = {};
   lim.max_response_fields = 10;
   CHECK(limited("{ items { id name } }", lim) == R"({"data":null,"errors":[{"message":"Response is larger than the limit allows."}]})");
   lim                    = {};
   lim.max_response_bytes = 64;
   CHECK(limited("{ squares(first: 1000) }", lim) == R"({"data":null,"errors":[{"message":"Response is larger than the limit allows."}]})");
   CHECK(limited("{ nothing nested { ok } items { name id } }", lim) == R"({"data":null,"errors":[{"message":"Response is larger than the limit allows."}]})");
}

// ── Robustness ──────────────────────────────────────────────────────────

TEST_CASE("graphql: mutated queries never crash or hang", "[graphql][fuzz]")
{
   const Query q = make_query(4);
   const std::string seeds[] = {
      "{ items { id name owner status tags { label weight } detail { __typename ... on Tag { label } ... on Extra { codes } } } }",
      "query Q($a: Int! = 4, $s: Boolean = true, $f: FilterInput) { add(a: $a, b: 2) @skip(if: $s) describe(filter: {names: [\"a\"], range: {lo: 1, hi: $a}}) ...F } fragment F on Query { squares(first: 3) node { id key } }",
      "{ accounts(first: 2, after: \"00\") { edges { cursor node { id title } } pageInfo { hasNextPage endCursor } } itemsView { rev best { label } } }",
      "{ __schema { types { name kind fields { name args { name type { kind name ofType { name } } } } } } __type(name: \"Item\") { name } }",
      "mutation M { bump(by: 1) reset value } query R { risky(ok: false) nested { bad } nullableList { ok bad } echo(text: \"\"\"block \\\"\"\" text\"\"\") }",
      "{ search { ... on Tag { v: label } ... on Extra { v: text } ...G } big(value: \"18446744073709551615\") later(at: \"2026-11-22T10:15:30Z\") } fragment G on Tag_or_Extra { __typename }",
   };
   const std::string variables[] = {
      "", "{}", R"({"a": 3, "s": false, "f": {"names": ["x"], "status": "active"}})", R"({"a": "x"})", R"({"s": [[[[1]]]]})",
      "null", "{", R"({"a": 1e99, "f": {"names": 5, "range": {"lo": 1}}})",
   };
   const std::string_view pieces[] = {
      "{", "}", "(", ")", "[", "]", ":", "!", "$", "@", "...", "\"", "\"\"\"", "#", ",", " ", "\n", "\\", "on", "query",
      "mutation", "fragment", "subscription", "true", "null", "@skip(if: true)", "@include(if: $s)", "$a", "items",
      "__typename", "__schema", "... on Item", "9999999999999999999999", "-0.0e-999", "\xff", "\xEF\xBB\xBF", "1e", "{ id }",
      "first: 2147483647", "$zz: [[Int!]!]! = [[1]]",
   };
   std::mt19937_64 rng{0x5EED'6071'9A11ull};
   gql::limits     lim;
   lim.max_complexity      = 20'000;
   lim.max_response_fields = 20'000;
   std::size_t ok = 0, rejected = 0;
   std::string out, text;
   for (int round = 0; round < 60000; ++round)
   {
      text = seeds[rng() % std::size(seeds)];
      const int edits = static_cast<int>(rng() % 4);  // a quarter stay valid
      for (int e = 0; e < edits && !text.empty(); ++e)
      {
         const std::size_t at = rng() % text.size();
         switch (rng() % 6)
         {
            case 0: text.erase(at, 1 + rng() % 8); break;
            case 1: text.insert(at, pieces[rng() % std::size(pieces)]); break;
            case 2: text[at] = static_cast<char>(rng()); break;
            case 3: text.insert(at, text.substr(rng() % text.size(), rng() % 40)); break;
            case 4: text.resize(at); break;
            default: std::swap(text[at], text[rng() % text.size()]); break;
         }
      }
      out.clear();
      Mutation    m;
      Session     session{"fuzz"};
      gql::request req{.operation_name = rng() % 8 == 0 ? "M" : "", .variables = variables[rng() % std::size(variables)]};
      req.user(session);
      const std::size_t errors = api::run(q, m, text, out, req, lim);
      INFO(text);
      REQUIRE(json_valid(out));
      REQUIRE((out.starts_with("{\"data\":") || out.starts_with("{\"errors\":")));
      REQUIRE(((errors == 0) == (out.find("\"errors\":[") == std::string::npos) || out.find("\"errors\":[") != std::string::npos));
      (errors == 0 ? ok : rejected) += 1;
   }
   // The mutations reach both outcomes.
   CHECK(ok > 1000);
   CHECK(rejected > 10000);
}

TEST_CASE("graphql: deeply nested and oversized input is refused, not recursed into", "[graphql][fuzz]")
{
   const Query q = make_query(1);
   std::string deep(200'000, '{');
   CHECK(first_message(run(q, deep)) == "Document is larger than the limit allows.");
   gql::limits lim;
   lim.max_document_bytes = 1 << 20;
   lim.max_tokens         = 1 << 20;
   CHECK(first_message(run(q, deep, {}, lim)) == "Syntax Error: Expected Name, found \"{\".");
   std::string nested;
   for (int i = 0; i < 5000; ++i)
      nested += "{ items ";
   CHECK(first_message(run(q, nested, {}, lim)) == "Selection sets are nested deeper than the limit allows.");
   std::string lists = "{ total(grid: " + std::string(100'000, '[') + ") }";
   CHECK(first_message(run(q, lists, {}, lim)) == "Input value is nested deeper than the limit allows.");
   std::string types = "query ($a: " + std::string(100'000, '[') + "Int) { nothing }";
   CHECK(first_message(run(q, types, {}, lim)) == "List types nest at most 7 deep.");
   const std::string vars = R"({"g": )" + std::string(100'000, '[') + "}";
   lim.max_variables_bytes = 1 << 20;
   CHECK(first_message(run(q, "query ($g: [[Int!]!]!) { total(grid: $g) }", gql::request{.variables = vars}, lim)) ==
         "Variables are not a valid JSON object.");
}

// The names of the time scalars: kind, unit and — unless it is a signed
// 64-bit count — the tick integer, as Rust's async-graphql scalars are named
// (rust/psio/src/graphql_scalars.rs).
TEST_CASE("graphql: time scalars are named per unit", "[graphql][time]")
{
   namespace c = std::chrono;
   using namespace psio::graphql::detail;
   auto name = []<typename T>() { return std::string_view{stored_name<duration_namer<T>>::value}; };
   using sys = c::system_clock;
   CHECK(name.operator()<c::time_point<sys, c::seconds>>() == "DateTimeSeconds");
   CHECK(name.operator()<c::time_point<sys, c::milliseconds>>() == "DateTimeMilliseconds");
   CHECK(name.operator()<c::time_point<sys, c::microseconds>>() == "DateTimeMicroseconds");
   CHECK(name.operator()<c::time_point<sys, c::nanoseconds>>() == "DateTimeNanoseconds");
   CHECK(name.operator()<c::time_point<sys, c::duration<std::int32_t, std::ratio<60>>>>() == "DateTime_60_1_i32");
   CHECK(name.operator()<c::time_point<sys, c::duration<std::uint32_t>>>() == "DateTimeSeconds_u32");
   CHECK(name.operator()<c::seconds>() == "Seconds");
   CHECK(name.operator()<c::milliseconds>() == "Milliseconds");
   CHECK(name.operator()<c::duration<std::int64_t, std::ratio<60>>>() == "Duration_60_1");
   CHECK(name.operator()<c::duration<std::uint32_t>>() == "Seconds_u32");
   CHECK(name.operator()<c::duration<std::int32_t, std::milli>>() == "Milliseconds_i32");
   CHECK(name.operator()<c::time_point<c::steady_clock, c::milliseconds>>() == "MonotonicMilliseconds");
   CHECK(name.operator()<c::time_point<c::steady_clock, c::duration<std::uint32_t, std::milli>>>() ==
         "MonotonicMilliseconds_u32");
   CHECK(name.operator()<c::time_point<c::steady_clock, c::duration<std::int64_t, std::ratio<60>>>>() ==
         "Monotonic_60_1");
}
