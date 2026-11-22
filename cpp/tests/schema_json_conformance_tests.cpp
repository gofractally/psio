// Consumes conformance/schema-json-vectors.json, the Schema IR text fixture
// shared with cargo test (rust/psio/tests/schema_json_conformance.rs) and the
// JS tests (js/src/test/schema-json-conformance.test.ts): psio::Schema as
// PSSZ bytes, psio::json text and psio::ijson text. Every row is checked
// against every typed entry point on every syntax layer and against the
// schema-driven codec over the IR of the IR.

#include "schema_json_conformance_types.hpp"

#include <catch.hpp>

#include <fstream>
#include <set>
#include <sstream>

using namespace schema_json_conf;
using namespace psio::schema_types;

namespace
{
   const Fixture& fixture()
   {
      static const Fixture fx = [] {
         std::ifstream     in(PSIO_SCHEMA_JSON_VECTORS, std::ios::binary);
         std::stringstream buf;
         buf << in.rdbuf();
         const std::string text = buf.str();
         Fixture           out;
         auto st = psio::decode_json_checked(psio::json{}, std::span<const char>{text}, out);
         if (!st.ok())
            FAIL("cannot read " PSIO_SCHEMA_JSON_VECTORS ": " << st.error().what);
         return out;
      }();
      return fx;
   }

   template <typename Fmt>
   void check_text(const std::string& text, const std::vector<char>& bytes)
   {
      INFO(Fmt::name);
      CHECK(write_schema_text<Fmt>(std::span<const char>{bytes}) == std::optional{text});
      bool       mixed = false;
      const auto back  = read_schema_text<Fmt>(text, &mixed);
      CHECK_FALSE(mixed);
      REQUIRE(back.has_value());
      CHECK(*back == bytes);
   }

   // One row's text under the format it names.
   template <typename Row, typename Check>
   void under_format(const Row& row, Check&& check)
   {
      if (row.format == "json")
         check(psio::json{});
      else if (row.format == "ijson")
         check(psio::ijson{});
      else
         FAIL("unknown format " << row.format);
   }
}  // namespace

TEST_CASE("schema json conformance: bytes and both texts of every schema", "[schema][json][ijson][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.version == 1);
   REQUIRE(fx.schemas.size() >= 7);
   std::set<std::string> names;
   for (const auto& row : fx.schemas)
   {
      INFO(row.name);
      CHECK(names.insert(row.name).second);
      const auto bytes = from_hex(row.ir_hex);
      REQUIRE(bytes.has_value());
      REQUIRE(psio::validate_strict<Schema>(psio::pssz{}, std::span<const char>{*bytes}).ok());
      // The PSSZ form is canonical.
      const Schema schema = psio::decode<Schema>(psio::pssz{}, std::span<const char>{*bytes});
      CHECK(psio::encode(psio::pssz{}, schema) == *bytes);
      check_text<psio::json>(row.json, *bytes);
      check_text<psio::ijson>(row.ijson, *bytes);
   }
   // The schemas the other fixtures record, the IR of the IR and the one
   // holding every alternative are all here.
   for (const char* name : {"pssz-vectors", "json-vectors", "time-key-vectors", "schema-of-schema",
                            "every-alternative"})
      CHECK(names.count(name) == 1);
}

TEST_CASE("schema json conformance: the hand-built schema holds every IR alternative",
          "[schema][json][ijson][conformance]")
{
   for (const auto& row : fixture().schemas)
   {
      if (row.name != "every-alternative")
         continue;
      const auto   bytes  = from_hex(row.ir_hex).value();
      const Schema schema = psio::decode<Schema>(psio::pssz{}, std::span<const char>{bytes});
      std::set<std::size_t> seen;
      for (const auto& member : schema.types)
         seen.insert(member.type->value.index());
      CHECK(seen.size() == std::variant_size_v<decltype(AnyType::value)>);
      CHECK_FALSE(schema.ir_version.has_value());
      CHECK_FALSE(schema.interfaces.empty());
      CHECK_FALSE(schema.worlds.empty());
      CHECK_FALSE(schema.uses.empty());
      CHECK_FALSE(schema.package.attributes.empty());
      // A 64-bit integer beyond 2^53 - 1: bare in json, a string in ijson.
      CHECK(row.json.find(R"("len":18446744073709551615)") != std::string::npos);
      CHECK(row.ijson.find(R"("len":"18446744073709551615")") != std::string::npos);
      CHECK(row.ijson.find(R"("len":9007199254740991)") != std::string::npos);
      CHECK(row.ijson.find(R"("len":"9007199254740992")") != std::string::npos);
      CHECK(row.ijson.find(R"("ir_version":null)") != std::string::npos);
      return;
   }
   FAIL("no every-alternative row");
}

TEST_CASE("schema json conformance: accepted texts read as the recorded schema",
          "[schema][json][ijson][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.accepts.size() >= 18);
   for (const auto& row : fx.accepts)
   {
      INFO(row.name);
      const auto bytes = from_hex(row.ir_hex);
      REQUIRE(bytes.has_value());
      under_format(row, [&]<typename Fmt>(Fmt) {
         bool       mixed = false;
         const auto back  = read_schema_text<Fmt>(row.text, &mixed);
         CHECK_FALSE(mixed);
         REQUIRE(back.has_value());
         CHECK(*back == *bytes);
      });
   }
}

TEST_CASE("schema json conformance: rejected texts are rejected by every reader",
          "[schema][json][ijson][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.rejects.size() >= 40);
   for (const auto& row : fx.rejects)
   {
      INFO(row.name);
      under_format(row, [&]<typename Fmt>(Fmt) {
         bool mixed = false;
         CHECK_FALSE(read_schema_text<Fmt>(row.text, &mixed).has_value());
         CHECK_FALSE(mixed);
      });
   }
}

TEST_CASE("schema json: a null Box has no text", "[schema][json][ijson]")
{
   Member      hollow{"m", Box<AnyType>{Int{8, false}}, {}};
   const auto  taken = std::move(hollow.type);
   std::string out   = "keep";
   CHECK_FALSE(psio::encode_json_checked(psio::ijson{}, hollow, out).ok());
   CHECK_FALSE(psio::encode_json_checked(psio::json{}, hollow, out).ok());
   CHECK(out == "keep");
   CHECK_THROWS_AS(psio::encode(psio::ijson{}, hollow), psio::codec_exception);
   // A Box is its value in text, as it is in pssz.
   CHECK(psio::encode(psio::ijson{}, taken) == psio::encode(psio::ijson{}, *taken));
   CHECK(psio::encode(psio::json{}, taken) == R"({"value":[8,{"bits":8,"isSigned":false}]})");
   CHECK(psio::encode(psio::ijson{}, taken) == R"({"value":{"Int":{"bits":8,"isSigned":false}}})");
}
