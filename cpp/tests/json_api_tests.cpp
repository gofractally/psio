// The parts of the JSON API (psio/json.hpp, docs/json.md "C++: views,
// documents built by hand, sinks") that are not "one typed value to or from
// a std::string": non-owning text and byte sources, untagged variants, the
// schemaless psio::json_value, typed reads from an open cursor, pretty
// printing and other sinks.

#include <psio/json.hpp>

#include <catch.hpp>

#include <psio/invariant.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
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

   // A row a hand-written reader fills: {"rowid":"7", <members of T>...}.
   struct Ev
   {
      std::uint32_t      inviteId = 0;
      std::string        event;
      std::optional<int> opt;
      PSIO_REFLECT(Ev, inviteId, event, opt)
   };
   template <typename T>
   struct SqlRow
   {
      std::uint64_t rowid = 0;
      T             data{};
      std::string   extra;  // a sub-document kept as text
   };
   template <typename Fmt, typename T, psio::JsonCursor C>
   bool read_row(psio::json_reader<Fmt, C>& r, SqlRow<T>& out)
   {
      C&               c   = r.cursor();
      bool             has = false, known = false;
      std::string_view key;
      if (!c.begin_object())
         return false;
      for (;;)
      {
         if (!c.next_field(has, key))
            return false;
         if (!has)
            return true;
         if (key == "rowid")
         {
            std::string text;
            if (!r.read(text))
               return false;
            out.rowid = std::strtoull(text.c_str(), nullptr, 10);
         }
         else if (key == "extra")
         {
            if (!r.raw(out.extra))
               return false;
         }
         else if (!r.read_member(key, out.data, known) || (!known && !r.skip()))
            return false;
      }
   }
   template <typename Fmt, psio::JsonCursor C>
   bool read_rows(psio::json_reader<Fmt, C>& r, std::vector<SqlRow<Ev>>& rows)
   {
      C&   c   = r.cursor();
      bool has = false;
      if (!c.begin_array())
         return false;
      for (;;)
      {
         if (!c.next_element(has))
            return false;
         if (!has)
            return c.finish();
         if (!read_row(r, rows.emplace_back()))
            return false;
      }
   }

   struct Positive
   {
      int value = 0;
      PSIO_REFLECT(Positive, transparent(), value)
   };
   inline psio::codec_status positive(const Positive& p) noexcept
   {
      return p.value > 0 ? psio::codec_ok()
                         : psio::codec_fail_owned("Positive: " + std::to_string(p.value) +
                                                     " is not positive",
                                                  0, "Positive");
   }
}  // namespace japi
PSIO_INVARIANT(japi::Positive, japi::positive, "Positive")

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

// ── Typed reads from an open cursor ────────────────────────────────────────
namespace
{
   const std::string_view rows_text =
      R"([{"rowid":"7","inviteId":3,"event":"x","extra":[1, {"a" : "b\u0041"} ],"unknown":{"k":[]}},)"
      R"( {"rowid":"8","event":"y","inviteId":4,"opt":5}])";

   template <typename Cursor>
   void check_rows(Cursor& c)
   {
      psio::json_reader             r{psio::ijson{}, c};
      std::vector<japi::SqlRow<japi::Ev>> rows;
      REQUIRE(japi::read_rows(r, rows));
      REQUIRE(r.status().ok());
      REQUIRE(rows.size() == 2);
      CHECK(rows[0].rowid == 7);
      CHECK(psio::to_ijson(rows[0].data) == R"({"inviteId":3,"event":"x","opt":null})");
      // The sub-document, compact, as PSIO writes it.
      CHECK(rows[0].extra == R"([1,{"a":"bA"}])");
      CHECK(rows[1].rowid == 8);
      CHECK(psio::to_ijson(rows[1].data) == R"({"inviteId":4,"event":"y","opt":5})");
   }
}  // namespace

TEST_CASE("json_reader: a hand-written reader delegates members and values",
          "[json][ijson][cursor]")
{
   {
      psio::json_text_cursor c{std::span<const char>{rows_text.data(), rows_text.size()}, true};
      check_rows(c);
   }
#if PSIO_HAVE_SIMDJSON
   {
      psio::simdjson_index_cursor c{std::span<const char>{rows_text.data(), rows_text.size()},
                                    true};
      check_rows(c);
   }
   {
      psio::simdjson_cursor c{std::span<const char>{rows_text.data(), rows_text.size()}, true};
      check_rows(c);
   }
#endif
}

TEST_CASE("json_reader: errors are the typed reader's", "[json][ijson][cursor]")
{
   auto run = [](std::string_view text, auto&& body) {
      psio::json_text_cursor c{std::span<const char>{text.data(), text.size()}, true};
      psio::json_reader      r{psio::ijson{}, c};
      body(r, c);
   };
   // A member's own rules apply: a string is not a u32.
   run(R"({"inviteId":"x"})", [](auto& r, auto& c) {
      japi::Ev         ev;
      bool             has = false, known = false;
      std::string_view key;
      REQUIRE(c.begin_object());
      REQUIRE(c.next_field(has, key));
      CHECK_FALSE(r.read_member(key, ev, known));
      CHECK(known);
      CHECK_FALSE(r.status().ok());
   });
   // An unknown member is not consumed.
   run(R"({"nope":[1,2]})", [](auto& r, auto& c) {
      japi::Ev         ev;
      bool             has = false, known = true;
      std::string_view key;
      REQUIRE(c.begin_object());
      REQUIRE(c.next_field(has, key));
      CHECK(r.read_member(key, ev, known));
      CHECK_FALSE(known);
      std::string text;
      CHECK(r.raw(text));
      CHECK(text == "[1,2]");
      REQUIRE(c.next_field(has, key));
      CHECK_FALSE(has);
      CHECK(c.finish());
   });
   // An invariant's own status, with its own text.
   run("-4", [](auto& r, auto&) {
      japi::Positive p;
      CHECK_FALSE(r.read(p));
      const auto st = r.status();
      REQUIRE_FALSE(st.ok());
      CHECK(st.error().what == "Positive: -4 is not positive");
      CHECK(st.error().format_name == "Positive");
   });
   run("[1,2", [](auto& r, auto&) {
      std::string text = "kept";
      CHECK_FALSE(r.raw(text));
      CHECK(text == "kept");
      CHECK_FALSE(r.status().ok());
      CHECK_FALSE(r.template validate<int>());
   });
   run("17", [](auto& r, auto& c) {
      CHECK(r.template validate<std::uint8_t>());
      CHECK(c.finish());
   });
}

TEST_CASE("json_text_cursor: the offset gives the text of a value as written",
          "[json][cursor]")
{
   const std::string_view text = R"({"a" : [1,  2 ] , "b":"x"})";
   psio::json_text_cursor c{std::span<const char>{text.data(), text.size()}, false};
   bool                   has = false;
   std::string_view       key;
   REQUIRE(c.begin_object());
   REQUIRE(c.next_field(has, key));
   REQUIRE(key == "a");
   psio::json_kind kind;
   REQUIRE(c.kind(kind));
   const std::size_t from = c.offset();
   REQUIRE(c.skip());
   CHECK(text.substr(from, c.offset() - from) == "[1,  2 ]");
   REQUIRE(c.next_field(has, key));
   REQUIRE(c.kind(kind));
   const std::size_t at = c.offset();
   REQUIRE(c.skip());
   CHECK(text.substr(at, c.offset() - at) == R"("x")");
   REQUIRE(c.next_field(has, key));
   CHECK_FALSE(has);
   CHECK(c.finish());
   CHECK(c.offset() == text.size());
}
