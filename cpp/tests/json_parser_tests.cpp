// The two JSON syntax layers (the hand-written lexer and simdjson) under the
// typed readers: both must accept the same texts and read the same values.
// Hand-picked texts aimed at where an on-demand parser is lenient, then a
// differential run over mutated documents.

#include <psio/json.hpp>

#include "wide_record_fixture.hpp"

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace
{
   struct Item
   {
      std::uint32_t              id = 0;
      std::string                name;
      std::optional<std::string> note;
      std::vector<std::uint8_t>  bytes;
      friend bool                operator==(const Item&, const Item&) = default;
   };
   PSIO_REFLECT(Item, id, name, note, bytes)

   using Choice = std::variant<std::uint32_t, std::string, Item>;

   struct Doc
   {
      std::uint64_t                         big = 0;
      std::int64_t                          neg = 0;
      double                                ratio = 0;
      bool                                  flag = false;
      std::string                           title;
      std::vector<Item>                     items;
      Choice                                choice;
      std::optional<std::int32_t>           maybe;
      std::tuple<std::uint16_t, std::string> pair;
      std::vector<std::vector<std::int32_t>> grid;
      friend bool                           operator==(const Doc&, const Doc&) = default;
   };
   PSIO_REFLECT(Doc, big, neg, ratio, flag, title, items, choice, maybe, pair, grid)

   Doc sample()
   {
      Doc d;
      d.big    = 18446744073709551615ull;
      d.neg    = -9007199254740993ll;
      d.ratio  = -12.5e-3;
      d.flag   = true;
      d.title  = "h\xC3\xA9llo \"q\" \\ \n \xF0\x9F\x98\x80 end";
      d.items  = {{1, "a", std::nullopt, {1, 2, 255}}, {2, "", "n\tote", {}}, {3, "ccc", "", {0}}};
      d.choice = Item{9, "nine", "x", {9}};
      d.maybe  = -5;
      d.pair   = {65535, "p"};
      d.grid   = {{1, -2}, {}, {3}};
      return d;
   }

   struct Outcome
   {
      bool        ok = false;
      std::string value;  // re-encoded, when ok
      bool        operator==(const Outcome&) const = default;
   };

   template <typename Fmt, typename T>
   Outcome run(std::string_view text, psio::json_parser parser, bool padded)
   {
      T                  out{};
      psio::codec_status st = psio::codec_ok();
      std::string        copy{text};
      if (padded)
         st = psio::decode_json_checked(Fmt{}, psio::padded_json::of(copy), out, parser);
      else
         st = psio::decode_json_checked(Fmt{}, std::span<const char>{copy}, out, parser);
      const bool valid = padded ? psio::validate_json<T>(Fmt{}, psio::padded_json::of(copy), parser).ok()
                                : psio::validate_json<T>(Fmt{}, std::span<const char>{copy}, parser).ok();
      REQUIRE(valid == st.ok());
      if (!st.ok())
         return {};
      std::string again;
      REQUIRE(psio::encode_json_checked(Fmt{}, out, again).ok());
      return {true, again};
   }

   // Both layers, padded and not, agree; returns the common outcome.
   template <typename Fmt, typename T>
   Outcome agree(std::string_view text)
   {
      const Outcome want = run<Fmt, T>(text, psio::json_parser::text, false);
      INFO("text: " << text);
      REQUIRE(run<Fmt, T>(text, psio::json_parser::simdjson, false) == want);
      REQUIRE(run<Fmt, T>(text, psio::json_parser::simdjson, true) == want);
      REQUIRE(run<Fmt, T>(text, psio::json_parser::simdjson_index, false) == want);
      REQUIRE(run<Fmt, T>(text, psio::json_parser::simdjson_index, true) == want);
      REQUIRE(run<Fmt, T>(text, psio::json_parser::text, true) == want);
      REQUIRE(run<Fmt, T>(text, psio::json_parser::automatic, false) == want);
      return want;
   }

   template <typename Fmt>
   void pointed()
   {
      using T = Item;
      auto ok   = [](std::string_view text) { return agree<Fmt, T>(text).ok; };
      const bool ij = Fmt::i_json;
      const std::string bytes = ij ? R"("0102")" : "[1,2]";
      auto item = [&](std::string_view head, std::string_view tail = "") {
         return std::string(head) + R"("id":1,"name":"n","bytes":)" + bytes + std::string(tail) + "}";
      };
      REQUIRE(ok(item("{")));
      REQUIRE(ok(item(" \t\r\n{ ")));
      REQUIRE(ok(item("{", R"(,"note":null)")));
      REQUIRE(ok(item("{", R"(,"note":"\u00e9\ud83d\ude00\n")")));
      // Unknown keys are skipped, and what is skipped must be JSON.
      REQUIRE(ok(item(R"({"x":[1,{"y":[true,false,null,-0.5e+3,"s"]}],)")));
      REQUIRE(!ok(item(R"({"x":[1 2],)")));
      REQUIRE(!ok(item(R"({"x":[1,],)")));
      REQUIRE(!ok(item(R"({"x":{"a" 1},)")));
      REQUIRE(!ok(item(R"({"x":{"a":1,},)")));
      REQUIRE(!ok(item(R"({"x":{1:2},)")));
      REQUIRE(!ok(item(R"({"x":nul,)")));
      REQUIRE(!ok(item(R"({"x":truex,)")));
      REQUIRE(!ok(item(R"({"x":01,)")));
      REQUIRE(!ok(item(R"({"x":1.,)")));
      REQUIRE(!ok(item(R"({"x":.5,)")));
      REQUIRE(!ok(item(R"({"x":+1,)")));
      REQUIRE(!ok(item(R"({"x":1e,)")));
      REQUIRE(!ok(item(R"({"x":-,)")));
      REQUIRE(!ok(item(R"({"x":0x10,)")));
      REQUIRE(!ok(item(R"({"x":NaN,)")));
      REQUIRE(!ok(item(R"({"x":Infinity,)")));
      REQUIRE(!ok(item(R"({"x":"a\qb",)")));
      REQUIRE(!ok(item(R"({"x":"\u12G4",)")));
      REQUIRE(!ok(item(R"({"x":"\ud800",)")));
      REQUIRE(!ok(item(R"({"x":"\udc00",)")));
      REQUIRE(!ok(item(R"({"x":"\ud800\u0041",)")));
      REQUIRE(ok(item(R"({"x":"\ud800\udc00",)")));
      REQUIRE(ok(item(R"({"x":"\u0000",)")));
      REQUIRE(!ok(item("{\"x\":\"a\tb\",")));
      REQUIRE(!ok(item("{\"x\":\"a\x01\",")));
      REQUIRE(!ok(item("{\"x\":\"\xC3\x28\",")));
      REQUIRE(!ok(item("{\"x\":\"\xED\xA0\x80\",")));
      REQUIRE(!ok(item("{\"x\":\"\xC0\x80\",")));
      // A token that does not end where the string or key ends.
      REQUIRE(!ok(item(R"({"x":"a"b,)")));
      REQUIRE(!ok(item(R"({"x"y:1,)")));
      REQUIRE(!ok(item(R"({"x":"a""b",)")));
      REQUIRE(!ok(item(R"({"x":1"b",)")));
      REQUIRE(!ok(item(R"({"x":truefalse,)")));
      REQUIRE(!ok(R"({"id":1,"name":"n"x,"bytes":[]})"));
      REQUIRE(!ok(R"({"id":1,"name"x:"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":1x,"name":"n","bytes":[]})"));
      // Keys ending in escaped backslashes and quotes.
      REQUIRE(ok(item(R"({"k\\":1,"k\"":2,"\\\\":3,"\\\"":4,)")));
      REQUIRE(ok(item("{", R"(,"note":"\\")")));
      REQUIRE(ok(item("{", R"(,"note":"\\\"")")));
      REQUIRE(ok(item("{", R"(,"note":"\"\\")")));
      // The I-JSON noncharacter rule, in read and in skipped strings and keys.
      REQUIRE(ok(item("{", ",\"note\":\"\xEF\xBF\xBE\"")) == !ij);
      REQUIRE(ok(item("{", R"(,"note":"\ufffe")")) == !ij);
      REQUIRE(ok(item("{\"x\":\"\xEF\xB7\x90\",")) == !ij);
      REQUIRE(ok(item("{\"\xF0\x9F\xBF\xBF\":1,")) == !ij);
      REQUIRE(ok(item(R"({"x":["\udbff\udfff"],)")) == !ij);
      // Document level.
      REQUIRE(!ok(""));
      REQUIRE(!ok("   "));
      REQUIRE(!ok("{"));
      REQUIRE(!ok(item("\xEF\xBB\xBF{")));
      REQUIRE(!ok(item("{") + "}"));
      REQUIRE(!ok(item("{") + " x"));
      REQUIRE(!ok(item("{") + "{}"));
      REQUIRE(!ok(item("{") + ","));
      REQUIRE(ok(item("{") + " \n"));
      REQUIRE(!ok(item("{").substr(0, item("{").size() - 1)));
      REQUIRE(!ok(item("{").substr(0, item("{").size() - 1) + "]"));
      REQUIRE(!ok(item("[")));
      REQUIRE(!ok(item("{,")));
      REQUIRE(!ok(R"({"id":1,"name":"n","bytes":[1,2})"));
      REQUIRE(!ok(R"({"id":1,,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":1 "name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":1,"name":"n","bytes":[]},)"));
      REQUIRE(!ok(R"({"id" 1,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({id:1,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({'id':1,"name":"n","bytes":[]})"));
      // Typed checks on top of either layer.
      REQUIRE(!ok(R"({"id":-0,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":1.0,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":1e0,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":4294967296,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":"1","name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":true,"name":"n","bytes":[]})"));
      REQUIRE(!ok(R"({"id":1,"name":null,"bytes":[]})"));
      REQUIRE(!ok(R"({"id":1,"name":7,"bytes":[]})"));
      REQUIRE(!ok(R"([1,"n",null,[]])"));
   }

   // Nesting: 64 levels are read, 65 are not, on both layers, for a typed
   // value and for a skipped one.
   template <typename Fmt>
   void depth()
   {
      for (const std::size_t levels : {63u, 64u, 65u, 200u})
      {
         std::string skipped = R"({"id":1,"name":"n","bytes":)";
         skipped += Fmt::i_json ? R"("")" : "[]";
         skipped += R"(,"x":)" + std::string(levels - 1, '[') + std::string(levels - 1, ']') + "}";
         INFO(levels);
         REQUIRE(agree<Fmt, Item>(skipped).ok == (levels <= 64));
      }
   }

   template <typename Fmt, typename T>
   void mutate(const T& value, unsigned seed, int rounds, std::size_t& accepted)
   {
      const std::string base = psio::encode(Fmt{}, value);
      REQUIRE(agree<Fmt, T>(base) == Outcome{true, base});
      std::mt19937                     rng(seed);
      static constexpr std::string_view alphabet =
         "{}[]:,\"\\ 0123456789-+.eEtrufalsn\t\n/xb\x01\x7f\xC3\xA9\xEF\xBF\xBE";
      for (int round = 0; round < rounds; ++round)
      {
         std::string text = base;
         const int   edits = 1 + static_cast<int>(rng() % 3);
         for (int e = 0; e < edits && !text.empty(); ++e)
         {
            const std::size_t at = rng() % text.size();
            switch (rng() % 5)
            {
               case 0: text[at] = alphabet[rng() % alphabet.size()]; break;
               case 1: text.erase(at, 1 + rng() % 3); break;
               case 2: text.insert(at, 1, alphabet[rng() % alphabet.size()]); break;
               case 3: text.resize(at); break;
               case 4:
               {
                  // Duplicate a slice: repeated keys and repeated elements.
                  const std::size_t len = 1 + rng() % 24;
                  text.insert(at, text.substr(at, len));
                  break;
               }
            }
         }
         if (agree<Fmt, T>(text).ok)
            ++accepted;
      }
   }
}  // namespace

TEST_CASE("json: both syntax layers accept and reject the same texts", "[json][parser]")
{
   pointed<psio::json>();
   depth<psio::json>();
}
TEST_CASE("ijson: both syntax layers accept and reject the same texts", "[ijson][parser]")
{
   pointed<psio::ijson>();
   depth<psio::ijson>();
}

TEST_CASE("both syntax layers agree on mutated documents", "[json][ijson][parser]")
{
   std::size_t accepted = 0;
   mutate<psio::json, Doc>(sample(), 1, 6000, accepted);
   mutate<psio::ijson, Doc>(sample(), 2, 6000, accepted);
   std::vector<Item> list(40, Item{7, "seven", "note", {1, 2, 3}});
   mutate<psio::json, std::vector<Item>>(list, 3, 3000, accepted);
   mutate<psio::ijson, std::vector<Item>>(list, 4, 3000, accepted);
   wide_fixture::Wide200 wide;
   wide.field_007 = 7;
   // Wide200 has no operator==; outcomes are compared by re-encoding.
   mutate<psio::json, wide_fixture::Wide200>(wide, 5, 1500, accepted);
   // Some mutations must survive, or the run compared rejections only.
   REQUIRE(accepted > 200);
}

TEST_CASE("the parser choice: automatic, forced, padded, and reentrant", "[json][parser]")
{
   // A large document: `automatic` takes the same path as `simdjson` when
   // simdjson is built in, and the value is the same either way.
   std::vector<Item> list(2000, Item{7, "seven", std::nullopt, {1, 2, 3}});
   const std::string text = psio::encode(psio::ijson{}, list);
   REQUIRE(text.size() > 50000);
   for (const auto parser : {psio::json_parser::automatic, psio::json_parser::text,
                             psio::json_parser::simdjson, psio::json_parser::simdjson_index})
   {
      std::vector<Item> out;
      REQUIRE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, out, parser)
                 .ok());
      REQUIRE(out == list);
      std::string padded = text;
      out.clear();
      REQUIRE(psio::decode_json_checked(psio::ijson{}, psio::padded_json::of(padded), out, parser)
                 .ok());
      REQUIRE(out == list);
      REQUIRE(padded == text);
   }
   REQUIRE(psio::decode<std::vector<Item>>(psio::ijson{}, std::span<const char>{text}) == list);
   REQUIRE(psio::validate<std::vector<Item>>(psio::ijson{}, std::span<const char>{text}).ok());
   // A document over the per-thread copy limit (1 MiB) and a small one after it.
   std::vector<Item> huge(60000, Item{1, "x", std::nullopt, {}});
   const std::string big = psio::encode(psio::json{}, huge);
   REQUIRE(big.size() > (1u << 20));
   std::vector<Item> out;
   REQUIRE(psio::decode_json_checked(psio::json{}, std::span<const char>{big}, out,
                                     psio::json_parser::simdjson)
              .ok());
   REQUIRE(out == huge);
   REQUIRE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, out,
                                     psio::json_parser::simdjson)
              .ok());
   REQUIRE(out == list);
#if PSIO_HAVE_SIMDJSON
   // What goes to simdjson: container roots, when no read is in progress.
   using psio::detail::json_impl::use_simdjson;
   REQUIRE(use_simdjson(std::span<const char>{text}, psio::json_parser::simdjson));
   REQUIRE(use_simdjson(std::span<const char>{text}, psio::json_parser::automatic));
   REQUIRE(!use_simdjson(std::span<const char>{text}, psio::json_parser::text));
   REQUIRE(!use_simdjson(std::span<const char>{std::string_view{"17"}},
                         psio::json_parser::simdjson));
   REQUIRE(!use_simdjson(std::span<const char>{std::string_view{"{}"}},
                         psio::json_parser::automatic));
   {
      // While a simdjson read is live on this thread, a nested read (as a
      // text adapter might start) runs on the hand-written lexer.
      psio::simdjson_cursor outer(std::span<const char>{text}, true);
      REQUIRE(!use_simdjson(std::span<const char>{text}, psio::json_parser::simdjson));
      std::vector<Item> nested;
      REQUIRE(psio::decode_json_checked(psio::ijson{}, std::span<const char>{text}, nested,
                                        psio::json_parser::simdjson)
                 .ok());
      REQUIRE(nested == list);
   }
   REQUIRE(use_simdjson(std::span<const char>{text}, psio::json_parser::simdjson));
#endif
}

TEST_CASE("wide integers are read exactly on both syntax layers", "[json][ijson][parser]")
{
   using L = std::vector<std::uint64_t>;
   using S = std::vector<std::int64_t>;
   // Strict json: bare integers beyond 2^53 and up to the type's range.
   const std::string_view bare =
      "[0,9007199254740991,9007199254740992,9007199254740993,18446744073709551615]";
   const L want{0, 9007199254740991ull, 9007199254740992ull, 9007199254740993ull,
                18446744073709551615ull};
   for (const auto parser : {psio::json_parser::text, psio::json_parser::simdjson,
                                psio::json_parser::simdjson_index})
   {
      L out;
      REQUIRE(psio::decode_json_checked(psio::json{}, std::span<const char>{bare}, out, parser)
                 .ok());
      REQUIRE(out == want);
      S neg;
      REQUIRE(psio::decode_json_checked(
                 psio::json{},
                 std::span<const char>{std::string_view{
                    "[-9223372036854775808,9223372036854775807,-9007199254740993]"}},
                 neg, parser)
                 .ok());
      REQUIRE(neg == S{std::numeric_limits<std::int64_t>::min(),
                       std::numeric_limits<std::int64_t>::max(), -9007199254740993ll});
   }
   // Out of range, and a float that a double-based reader would round into range.
   REQUIRE(!agree<psio::json, L>("[18446744073709551616]").ok);
   REQUIRE(!agree<psio::json, L>("[1.8446744073709551615e19]").ok);
   REQUIRE(!agree<psio::json, L>("[100000000000000000000]").ok);
   REQUIRE(!agree<psio::json, S>("[9223372036854775808]").ok);
   REQUIRE(!agree<psio::json, S>("[-9223372036854775809]").ok);
   REQUIRE(!agree<psio::json, L>("[-1]").ok);
   // I-JSON: bare within the safe range, a string beyond it.
   REQUIRE(agree<psio::ijson, L>(R"([9007199254740991,"9007199254740992","18446744073709551615","7"])") ==
           Outcome{true, R"([9007199254740991,"9007199254740992","18446744073709551615",7])"});
   REQUIRE(!agree<psio::ijson, L>("[9007199254740992]").ok);
   REQUIRE(!agree<psio::ijson, L>(R"(["18446744073709551616"])").ok);
   REQUIRE(!agree<psio::ijson, L>(R"(["\u0037"])").ok);
   REQUIRE(!agree<psio::ijson, L>(R"(["07"])").ok);
   REQUIRE(!agree<psio::ijson, S>(R"(["-0"])").ok);
   REQUIRE(agree<psio::ijson, S>(R"(["-9223372036854775808",-9007199254740991])") ==
           Outcome{true, R"(["-9223372036854775808",-9007199254740991])"});
}

TEST_CASE("floating-point numbers are read identically on both syntax layers",
          "[json][ijson][parser]")
{
   using D = std::vector<double>;
   using F = std::vector<float>;
   for (const std::string_view number :
        {"0", "-0", "0.0", "-0.0", "1", "-1", "1e0", "1E+2", "1e-2", "0e0", "0.1", "0.30000000000000004",
         "123456789012345678901234567890", "9007199254740993", "18446744073709551615",
         "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1e308",
         "1e309", "-1e309", "1e400", "4.9e-324", "2.4703282292062327e-324", "2.4703282292062328e-324",
         "1e-400", "-1e-400", "2.2250738585072011e-308", "2.2250738585072014e-308",
         "3.4028234663852886e38", "3.4028235677973366e38", "3.4028236e38", "1.401298464324817e-45",
         "7.006492321624085e-46", "16777217", "0.1e1", "1.0e+0", "1.e1", ".1", "1e", "1e+", "01", "-",
         "+1", "1.5.2", "0x1p3", "NaN", "Infinity", "-Infinity", "1_000", "1 ", " 1", "1,", "\"1\""})
   {
      const std::string text = "[" + std::string(number) + "]";
      INFO(text);
      (void)agree<psio::json, D>(text);
      (void)agree<psio::ijson, D>(text);
      (void)agree<psio::json, F>(text);
      (void)agree<psio::ijson, F>(text);
   }
   REQUIRE(agree<psio::json, D>("[1e-400]") == Outcome{true, "[0]"});
   REQUIRE(agree<psio::json, D>("[-0.0]") == Outcome{true, "[-0]"});
   REQUIRE(!agree<psio::json, D>("[1e309]").ok);
   REQUIRE(agree<psio::json, D>("[9007199254740993]") == Outcome{true, "[9007199254740992]"});
}
