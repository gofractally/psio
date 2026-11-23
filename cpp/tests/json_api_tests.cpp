// The parts of the JSON API (psio/json.hpp, docs/json.md "C++: views,
// documents built by hand, sinks") that are not "one typed value to or from
// a std::string": non-owning text and byte sources, untagged variants, the
// schemaless psio::json_value, typed reads from an open cursor, pretty
// printing and other sinks.

#include <psio/json.hpp>

#include <catch.hpp>

#include <psio/invariant.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>
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

   // "Any JSON here" as members of a reflected record.
   struct Pretty
   {
      std::string                     service;
      psio::json_value                data;
      std::optional<psio::json_value> more;
      std::vector<psio::json_value>   list;
      PSIO_REFLECT(Pretty, service, data, more, list)
   };
   // The same bytes: the JSON texts as UTF-8 strings.
   struct PrettyText
   {
      std::string                      service;
      std::string                      data;
      std::optional<std::string>       more;
      std::vector<std::string>         list;
      PSIO_REFLECT(PrettyText, service, data, more, list)
   };
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

// ── psio::json_value: any JSON value ───────────────────────────────────────
TEST_CASE("json_value: a document read and written again is the same text",
          "[json][ijson][json_value]")
{
   // Numbers keep their text: nothing is rounded, renormalised or widened.
   const std::string_view text =
      R"({"a":1.50,"b":"x\n","c":[1,2,{"d":null}],"big":18446744073709551616,"e":1e3,"neg":-0,"z":true,"o":{}})";
   for (const bool ijson : {false, true})
   {
      psio::json_value v;
      REQUIRE((ijson ? psio::decode_json_checked(psio::ijson{}, text, v)
                     : psio::decode_json_checked(psio::json{}, text, v))
                 .ok());
      CHECK((ijson ? psio::to_ijson(v) : psio::to_json(v)) == text);
      REQUIRE(v.is_object());
      CHECK(v.as_object()->size() == 8);
      CHECK(v.find("a")->as_number()->text == "1.50");
      CHECK(v.find("a")->as_number()->as_double() == 1.5);
      CHECK_FALSE(v.find("a")->as_number()->as_int64());
      CHECK(v.find("big")->as_number()->text == "18446744073709551616");
      CHECK_FALSE(v.find("big")->as_number()->as_uint64());
      CHECK(*v.find("b")->as_string() == "x\n");
      CHECK(v.find("c")->as_array()->size() == 3);
      CHECK((*v.find("c")->as_array())[1].as_number()->as_int64() == 2);
      CHECK((*v.find("c")->as_array())[2].find("d")->is_null());
      CHECK(*v.find("z")->as_bool());
      CHECK(v.find("missing") == nullptr);
      CHECK(v.find("o")->as_object()->empty());
   }
   CHECK(psio::from_ijson<psio::json_value>(" [ 1 , \"a\" ] ") ==
         psio::json_value{psio::json_value::array{1, "a"}});
   // Scalars at the root.
   CHECK(psio::to_ijson(psio::from_ijson<psio::json_value>("null")) == "null");
   CHECK(psio::to_ijson(psio::from_ijson<psio::json_value>("-12.5e-3")) == "-12.5e-3");

   // Not JSON: an error, on every reading entry point.
   for (std::string_view bad : {R"({"a":1,})", "[1 2]", "", "01", R"({"a":1}x)", "nul"})
   {
      INFO(bad);
      psio::json_value v;
      CHECK_FALSE(psio::decode_json_checked(psio::ijson{}, bad, v).ok());
      CHECK_FALSE(psio::validate_json<psio::json_value>(
                     psio::json{}, std::span<const char>{bad.data(), bad.size()})
                     .ok());
   }
   // Nesting is bounded like every reader's.
   const std::string deep(psio::kMaxValidationDepth + 1, '[');
   psio::json_value   v;
   CHECK_FALSE(psio::decode_json_checked(psio::json{}, std::string_view{deep}, v).ok());
}

TEST_CASE("json_value: a repeated key keeps its last value, at the first position",
          "[json][ijson][json_value]")
{
   const auto v = psio::from_ijson<psio::json_value>(R"({"a":1,"b":2,"a":{"x":[]},"b":3})");
   CHECK(psio::to_ijson(v) == R"({"a":{"x":[]},"b":3})");
   REQUIRE(v.as_object()->size() == 2);
   // A hand-built object that holds a key twice is written with the last.
   psio::json_value built{psio::json_value::object{{"k", 1}, {"j", 2}, {"k", "last"}}};
   CHECK(*built.find("k")->as_string() == "last");
   CHECK(psio::to_json(built) == R"({"j":2,"k":"last"})");
}

TEST_CASE("json_value: built and edited in code", "[json][ijson][json_value]")
{
   psio::json_value v;
   CHECK(v.is_null());
   v.set("service", "tokens");
   v.set("amount", std::uint64_t{18446744073709551615ull});
   v.set("ratio", 0.25);
   v.set("ok", true);
   v.set("none", nullptr);
   v.set("list", psio::json_value::array{1, -2, "three"});
   v.set("amount", 5);  // replaced where it is
   CHECK(psio::to_ijson(v) ==
         R"({"service":"tokens","amount":5,"ratio":0.25,"ok":true,"none":null,"list":[1,-2,"three"]})");
   CHECK(v.erase("none") == 1);
   CHECK(v.erase("none") == 0);
   v.find("list")->as_array()->pop_back();
   CHECK(psio::to_json(v) == R"({"service":"tokens","amount":5,"ratio":0.25,"ok":true,"list":[1,-2]})");
   // A number is its text; a json_value does not know its width, so ijson
   // writes it as it is.
   CHECK(psio::to_ijson(psio::json_value{std::uint64_t{18446744073709551615ull}}) ==
         "18446744073709551615");
   CHECK(psio::json_number{std::int64_t{-7}}.as_int64() == -7);
   CHECK_FALSE(psio::json_number{std::int64_t{-7}}.as_uint64());

   // A value with no JSON text is an error, not output.
   std::string out = "kept";
   auto        st  = psio::encode_json_checked(psio::ijson{},
                                               psio::json_value{psio::json_number{"0x10"}}, out);
   REQUIRE_FALSE(st.ok());
   CHECK(st.error().what == "json: a json_value number is not a JSON number");
   CHECK(out == "kept");
   CHECK_FALSE(psio::encode_json_checked(psio::json{},
                                         psio::json_value{std::numeric_limits<double>::infinity()},
                                         out)
                  .ok());
   CHECK_FALSE(psio::encode_json_checked(psio::json{}, psio::json_value{std::string{"\xff"}}, out)
                  .ok());
   // The I-JSON string rule is the only difference between the formats.
   const psio::json_value nonchar{std::string{"\xEF\xBF\xBF"}};
   CHECK(psio::encode_json_checked(psio::json{}, nonchar, out).ok());
   CHECK_FALSE(psio::encode_json_checked(psio::ijson{}, nonchar, out).ok());
   CHECK_THROWS_AS(psio::to_ijson(nonchar), psio::codec_exception);
}

TEST_CASE("json_value: a member of a reflected record, in text and in PSSZ",
          "[json][ijson][pssz][schema][json_value]")
{
   namespace ir = psio::pssz_ir;
   const std::string_view text =
      R"({"service":"tokens","data":{"amount":"5","memo":[1.0,null]},"more":7,"list":[true,"x",{}]})";
   japi::Pretty p;
   REQUIRE(psio::decode_json_checked(psio::ijson{}, text, p).ok());
   CHECK(psio::to_ijson(p) == text);
   CHECK(*p.data.find("amount")->as_string() == "5");
   // A missing optional member is none; a missing json_value is an error
   // (null is a value: it has to be written).
   japi::Pretty q;
   REQUIRE(psio::decode_json_checked(psio::ijson{}, R"({"service":"s","data":null,"list":[]})", q)
              .ok());
   CHECK(q.data.is_null());
   CHECK_FALSE(q.more.has_value());
   CHECK_FALSE(psio::decode_json_checked(psio::ijson{}, R"({"service":"s","list":[]})", q).ok());

   // PSSZ: each json_value is a UTF-8 string holding its compact text.
   const japi::PrettyText twin{"tokens", R"({"amount":"5","memo":[1.0,null]})", "7",
                               {"true", R"("x")", "{}"}};
   const auto bytes = psio::encode(psio::pssz{}, p);
   CHECK(bytes == psio::encode(psio::pssz{}, twin));
   REQUIRE(psio::validate<japi::Pretty>(psio::pssz{}, std::span<const char>{bytes}).ok());
   REQUIRE(psio::validate_strict<japi::Pretty>(psio::pssz{}, std::span<const char>{bytes}).ok());
   CHECK(psio::to_ijson(psio::decode<japi::Pretty>(psio::pssz{}, std::span<const char>{bytes})) ==
         text);
   // Bytes that are not JSON text are not a value; text that is JSON but
   // not compact is a value with another, canonical, encoding.
   auto bad = twin;
   bad.data = "{not json";
   const auto bad_bytes = psio::encode(psio::pssz{}, bad);
   CHECK_FALSE(psio::validate<japi::Pretty>(psio::pssz{}, std::span<const char>{bad_bytes}).ok());
   auto loose = twin;
   loose.data = R"({ "amount" : "5" })";
   const auto loose_bytes = psio::encode(psio::pssz{}, loose);
   CHECK(psio::validate<japi::Pretty>(psio::pssz{}, std::span<const char>{loose_bytes}).ok());
   CHECK_FALSE(
      psio::validate_strict<japi::Pretty>(psio::pssz{}, std::span<const char>{loose_bytes}).ok());

   // The schema: Custom "json" over a UTF-8 string; and the schema-driven
   // codecs agree with the typed ones on bytes and on text.
   const auto schema = psio::SchemaBuilder{}.insert<japi::Pretty>("Pretty").build();
   REQUIRE(ir::validate_schema(schema).ok());
   const auto& record = std::get<psio::schema_types::Object>(schema.get("Pretty")->value);
   const auto* node   = record.members[1].type->resolve(schema);
   REQUIRE(node);
   const auto& custom = std::get<psio::schema_types::Custom>(node->value);
   CHECK(custom.id == "json");
   CHECK(std::get<psio::schema_types::Custom>(custom.type->value).id == "utf8");
   for (const bool ijson : {false, true})
   {
      INFO(ijson);
      const std::string typed = ijson ? psio::to_ijson(p) : psio::to_json(p);
      std::string       out;
      REQUIRE((ijson ? ir::pssz_to_json(schema, "Pretty", psio::ijson{},
                                        std::span<const char>{bytes}, out)
                     : ir::pssz_to_json(schema, "Pretty", psio::json{},
                                        std::span<const char>{bytes}, out))
                 .ok());
      CHECK(out == typed);
      std::vector<char> back;
      REQUIRE((ijson ? ir::json_to_pssz(schema, "Pretty", psio::ijson{}, std::string_view{typed},
                                        back)
                     : ir::json_to_pssz(schema, "Pretty", psio::json{}, std::string_view{typed},
                                        back))
                 .ok());
      CHECK(back == bytes);
   }
   CHECK(ir::validate_pssz(schema, "Pretty", std::span<const char>{bytes}).ok());
   CHECK_FALSE(ir::validate_pssz(schema, "Pretty", std::span<const char>{bad_bytes}).ok());
   // Loose text in the bytes is presented compact.
   std::string out;
   REQUIRE(ir::pssz_to_json(schema, "Pretty", psio::ijson{}, std::span<const char>{loose_bytes},
                            out)
              .ok());
   CHECK(out == text);
   // No handler may claim the id.
   ir::CustomHandlers handlers;
   CHECK_FALSE(handlers.try_add("json", {}).ok());
}
