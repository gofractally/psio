// The parts of the JSON API (psio/json.hpp, docs/json.md "C++: views,
// documents built by hand, sinks") that are not "one typed value to or from
// a std::string": non-owning text and byte sources, untagged variants, the
// schemaless psio::json_value, typed reads from an open cursor, pretty
// printing and other sinks.

#include <psio/json.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace japi
{
   struct Views
   {
      std::string_view      name;
      const char*           note = nullptr;
      std::span<const char> raw;
      PSIO_REFLECT(Views, name, note, raw)
   };
}  // namespace japi

// ── Non-owning sources have text (writers) ─────────────────────────────────
TEST_CASE("json: std::string_view, C strings and literals are written as strings",
          "[json][ijson][views]")
{
   std::string out;
   psio::to_ijson(std::string_view{"x"}, out);  // (the probe of the gap)
   CHECK(out == R"("x")");
   CHECK(psio::to_json(std::string_view{"a\"b"}) == R"("a\"b")");
   CHECK(psio::to_ijson("lit\n") == R"("lit\n")");
   CHECK(psio::to_json("lit") == R"("lit")");
   const char* c = "cstr";
   CHECK(psio::to_ijson(c) == R"("cstr")");
   char buffer[8] = "ab";  // a char array is its text up to the terminator
   CHECK(psio::to_ijson(buffer) == R"("ab")");
   CHECK(psio::to_ijson(std::tuple{std::string_view{"a"}, "b", 3}) == R"(["a","b",3])");
   CHECK(psio::to_ijson(std::vector<std::string_view>{"k", "v"}) == R"(["k","v"])");

   // The rules of a string apply: UTF-8, and no noncharacter under ijson.
   std::string text;
   CHECK_FALSE(psio::encode_json_checked(psio::json{}, std::string_view{"\xff"}, text).ok());
   CHECK(text.empty());
   CHECK_FALSE(
      psio::encode_json_checked(psio::ijson{}, std::string_view{"\xEF\xBF\xBF"}, text).ok());
   CHECK(psio::encode_json_checked(psio::json{}, std::string_view{"\xEF\xBF\xBF"}, text).ok());
   // A null C string is no string.
   const char* null = nullptr;
   text.clear();
   const auto st = psio::encode_json_checked(psio::ijson{}, null, text);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == "json: null C string");
   CHECK_THROWS_AS(psio::to_ijson(null), psio::codec_exception);
}

TEST_CASE("json: a span is written as the list, or the bytes, it views", "[json][ijson][views]")
{
   const std::array<int, 3> numbers{1, 2, 3};
   CHECK(psio::to_json(std::span<const int>{numbers}) == "[1,2,3]");
   CHECK(psio::to_ijson(std::span<const int>{numbers}) == "[1,2,3]");
   const std::vector<unsigned char> bytes{0x01, 0xAB};
   CHECK(psio::to_ijson(std::span<const unsigned char>{bytes}) == R"("01AB")");
   CHECK(psio::to_json(std::span<const unsigned char>{bytes}) == "[1,171]");
   CHECK(psio::to_ijson(std::span<const unsigned char>{bytes}) == psio::to_ijson(bytes));
   CHECK(psio::to_json(std::span<const unsigned char>{bytes}) == psio::to_json(bytes));
   const std::array<std::byte, 2> raw{std::byte{0}, std::byte{0xFF}};
   CHECK(psio::to_ijson(std::span<const std::byte, 2>{raw}) == R"("00FF")");

   // As members of a record that is only ever written.
   const char data[2] = {0x01, 0x7F};
   const japi::Views v{"n", "z", std::span<const char>{data}};
   CHECK(psio::to_ijson(v) == R"({"name":"n","note":"z","raw":"017F"})");
   CHECK(psio::to_json(v) == R"({"name":"n","note":"z","raw":[1,127]})");
}
