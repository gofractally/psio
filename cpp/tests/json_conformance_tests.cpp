// Consumes conformance/json-vectors.json, the JSON / I-JSON fixture shared
// with cargo test (rust/psio/tests/json_conformance.rs). Every row is checked
// against every reader and writer entry point of psio::json / psio::ijson.

#include "json_conformance_types.hpp"

#include <catch.hpp>

#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

using namespace json_conf;

namespace
{
   const Fixture& fixture()
   {
      static const Fixture fx = [] {
         std::ifstream     in(PSIO_JSON_VECTORS, std::ios::binary);
         std::stringstream buf;
         buf << in.rdbuf();
         const std::string text = buf.str();
         Fixture           out;
         auto st = psio::decode_json_checked(psio::json{}, std::span<const char>{text}, out);
         if (!st.ok())
            FAIL("cannot read " PSIO_JSON_VECTORS ": " << st.error().what << " at "
                                                       << st.error().byte_offset);
         return out;
      }();
      return fx;
   }

   using Dispatch = std::map<std::string, std::function<std::string(const Row&)>>;
   const Dispatch& dispatch()
   {
      static const Dispatch table = [] {
         Dispatch out;
         for_each_type([&]<typename T>(std::type_identity<T>) {
            out.emplace(type_id<T>(), [](const Row& row) { return check_row<T>(row); });
         });
         return out;
      }();
      return table;
   }
}  // namespace

TEST_CASE("json conformance: every vector holds on every entry point", "[json][ijson][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.version == 1);
   REQUIRE(fx.vectors.size() >= 400);
   std::set<std::string> names;
   for (const auto& row : fx.vectors)
   {
      INFO(row.name);
      CHECK(names.insert(row.name).second);
      const auto it = dispatch().find(row.type);
      REQUIRE(it != dispatch().end());
      CHECK(it->second(row) == "");
   }
}

TEST_CASE("json conformance: the fixture covers every type, kind and format",
          "[json][ijson][conformance]")
{
   const auto&                                  fx = fixture();
   std::map<std::string, std::set<std::string>> kinds_of_type;
   std::map<std::string, std::size_t>           count;
   for (const auto& row : fx.vectors)
   {
      kinds_of_type[row.type].insert(row.kind);
      ++count[row.kind + ":" + row.format.value_or("both")];
   }
   for (const auto& [id, fn] : dispatch())
   {
      INFO(id);
      CHECK(kinds_of_type[id].count("value") == 1);
   }
   CHECK(count["value:both"] >= 100);
   CHECK(count["accept:json"] >= 10);
   CHECK(count["accept:ijson"] >= 10);
   CHECK(count["reject:json"] >= 100);
   CHECK(count["reject:ijson"] >= 150);
}

TEST_CASE("json conformance: the two formats differ exactly where the rules say",
          "[json][ijson][conformance]")
{
   // A value whose type has no wide integer, byte sequence or variant has
   // one text in both formats.
   const std::set<std::string> same{"bool", "u8", "i8", "u16", "i16", "u32", "i32", "f32", "f64",
                                    "string", "raw_string", "list<u32>", "array<u16,3>",
                                    "optional<string>", "tuple<>", "AllOptional", "Version",
                                    "Release", "Tree", "Account"};
   for (const auto& row : fixture().vectors)
   {
      if (row.kind != "value") continue;
      INFO(row.name);
      if (same.count(row.type)) CHECK(*row.json == *row.ijson);
   }
}
