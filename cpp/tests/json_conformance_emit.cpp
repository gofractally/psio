// Emits conformance/json-vectors.json, the shared JSON / I-JSON fixture
// consumed by ctest (json_conformance_tests.cpp) and cargo test
// (rust/psio/tests/json_conformance.rs).
//
//   psio_json_conformance_emit <out.json>      write the fixture
//   psio_json_conformance_emit --check <ref>   fail if <ref> is not what the
//                                               implementation generates
//
// "value" rows are computed from C++ values. "accept" and "reject" rows are
// hand-written texts; the emitter aborts when the implementation disagrees
// with the stated outcome, so the file never records an accident.

#include "json_conformance_types.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

using namespace json_conf;

namespace
{
   std::vector<Row>      rows;
   std::set<std::string> names;

   [[noreturn]] void die(const std::string& what)
   {
      std::fprintf(stderr, "json conformance emit: %s\n", what.c_str());
      std::exit(1);
   }

   template <typename T>
   void push(Row row)
   {
      row.type = type_id<T>();
      row.name = row.kind + "/" + row.type + "/" + row.name;
      if (row.format) row.name += "/" + *row.format;
      if (!names.insert(row.name).second) die("duplicate row " + row.name);
      if (auto e = check_row<T>(row); !e.empty()) die(row.name + ": " + e);
      rows.push_back(std::move(row));
   }

   // One value, both texts.
   template <typename T>
   void value(const std::string& name, const T& v)
   {
      Row row;
      row.name  = name;
      row.kind  = "value";
      row.json  = psio::encode(psio::json{}, v);
      row.ijson = psio::encode(psio::ijson{}, v);
      T back{};
      if (!psio::decode_json_checked(psio::ijson{}, std::span<const char>{*row.ijson}, back).ok() ||
          !(back == v))
         die(name + ": ijson round trip");
      if (!psio::decode_json_checked(psio::json{}, std::span<const char>{*row.json}, back).ok() ||
          !(back == v))
         die(name + ": json round trip");
      push<T>(std::move(row));
   }

   bool is_utf8(const std::string& s)
   {
      std::string out;
      return psio::encode_json_checked(psio::json{}, s, out).ok();
   }

   void set_text(Row& row, const std::string& text)
   {
      if (is_utf8(text))
         row.text = text;
      else
      {
         static const char hex[] = "0123456789abcdef";
         std::string       h;
         for (unsigned char c : text)
         {
            h.push_back(hex[c >> 4]);
            h.push_back(hex[c & 15]);
         }
         row.text_hex = h;
      }
   }

   template <typename T>
   void accept(const char* format, const std::string& name, const std::string& text,
               const std::string& canonical)
   {
      Row row;
      row.name      = name;
      row.kind      = "accept";
      row.format    = format;
      row.canonical = canonical;
      set_text(row, text);
      push<T>(std::move(row));
   }
   template <typename T>
   void accept_both(const std::string& name, const std::string& text, const std::string& canonical)
   {
      accept<T>("json", name, text, canonical);
      accept<T>("ijson", name, text, canonical);
   }

   template <typename T>
   void reject(const char* format, const std::string& name, const std::string& text)
   {
      Row row;
      row.name   = name;
      row.kind   = "reject";
      row.format = format;
      set_text(row, text);
      push<T>(std::move(row));
   }
   template <typename T>
   void reject_both(const std::string& name, const std::string& text)
   {
      reject<T>("json", name, text);
      reject<T>("ijson", name, text);
   }

   // A JSON unicode escape: backslash, 'u', four hex digits.
   std::string u(const char* hex4) { return std::string(1, '\\') + "u" + hex4; }

   std::string repeat(const std::string& s, std::size_t n)
   {
      std::string out;
      for (std::size_t i = 0; i < n; ++i) out += s;
      return out;
   }
   std::string tree_text(std::size_t levels)
   {
      return repeat(R"({"children":[)", levels) + R"({"children":[]})" + repeat("]}", levels);
   }
   Tree tree_value(std::size_t levels)
   {
      Tree t;
      for (std::size_t i = 0; i < levels; ++i)
      {
         Tree parent;
         parent.children.push_back(std::move(t));
         t = std::move(parent);
      }
      return t;
   }

   using u8  = std::uint8_t;
   using i8  = std::int8_t;
   using u16 = std::uint16_t;
   using i16 = std::int16_t;
   using u32 = std::uint32_t;
   using i32 = std::int32_t;
   using u64 = std::uint64_t;
   using i64 = std::int64_t;
   template <typename T>
   using Opt = std::optional<T>;
   template <typename T>
   using Vec = std::vector<T>;

   const Inner inner1{7, text("seven"), true};
   const Inner inner_big{18446744073709551615ull, text("max \xE2\x98\x83"), false};

   void emit_scalars()
   {
      value<bool>("true", true);
      value<bool>("false", false);
      value<u8>("zero", 0);
      value<u8>("max", 255);
      value<i8>("min", -128);
      value<i8>("max", 127);
      value<u16>("max", 65535);
      value<i16>("min", -32768);
      value<u32>("zero", 0);
      value<u32>("max", 4294967295u);
      value<i32>("min", std::numeric_limits<i32>::min());
      value<i32>("max", std::numeric_limits<i32>::max());

      value<u64>("zero", 0);
      value<u64>("one", 1);
      value<u64>("safe_max_2^53-1", (u64{1} << 53) - 1);
      value<u64>("2^53", u64{1} << 53);
      value<u64>("2^53+1", (u64{1} << 53) + 1);
      value<u64>("max", std::numeric_limits<u64>::max());
      value<i64>("zero", 0);
      value<i64>("minus_one", -1);
      value<i64>("min", std::numeric_limits<i64>::min());
      value<i64>("max", std::numeric_limits<i64>::max());
      value<i64>("2^53", i64{1} << 53);
      value<i64>("2^53+1", (i64{1} << 53) + 1);
      value<i64>("-2^53", -(i64{1} << 53));
      value<i64>("-(2^53+1)", -((i64{1} << 53) + 1));

      value<float>("zero", 0.0f);
      value<float>("one_and_half", 1.5f);
      value<float>("minus_tenth", -0.1f);
      value<float>("max", std::numeric_limits<float>::max());
      value<float>("denorm_min", std::numeric_limits<float>::denorm_min());
      value<float>("2^24", 16777216.0f);
      value<float>("1e10", 1e10f);
      value<float>("1e-5", 1e-5f);
      value<double>("zero", 0.0);
      value<double>("minus_zero", -0.0);
      value<double>("one", 1.0);
      value<double>("tenth", 0.1);
      value<double>("third", 1.0 / 3.0);
      value<double>("hundred", 100.0);
      value<double>("1e5", 1e5);
      value<double>("123456", 123456.0);
      value<double>("1e21", 1e21);
      value<double>("1e22", 1e22);
      value<double>("1.5e300", 1.5e300);
      value<double>("1e-7", 1e-7);
      value<double>("0.001", 0.001);
      value<double>("0.0001234", 0.0001234);
      value<double>("1234e5", 123400000.0);
      value<double>("12345678901234567890", 12345678901234567890.0);
      value<double>("2^53", 9007199254740992.0);
      value<double>("denorm_min", std::numeric_limits<double>::denorm_min());
      value<double>("max", std::numeric_limits<double>::max());
      value<double>("minus_pi", -3.141592653589793);
      value<double>("1e100", 1e100);
      value<double>("1.25e-100", 1.25e-100);
   }

   void emit_strings_and_bytes()
   {
      const std::vector<std::pair<std::string, std::string>> texts{
         {"empty", ""},
         {"ascii", "hello"},
         {"quote_backslash", "quote\" back\\slash"},
         {"newline_tab_cr", "line\nfeed\ttab\rcr"},
         {"controls", std::string("\x01\x08\x0c\x1f", 4)},
         {"nul", std::string("a\0b", 3)},
         {"del_and_slash", "\x7f/"},
         {"two_byte", "h\xC3\xA9llo"},
         {"three_byte", "snow \xE2\x98\x83"},
         {"four_byte", "smile \xF0\x9F\x98\x80"},
         {"max_code_point_plane_16", "\xF4\x8F\xBF\xBD"},
      };
      for (const auto& [name, s] : texts)
      {
         value<Text>(name, text(s));
         value<std::string>(name, s);
      }
      value<Bytes>("empty", {});
      value<Bytes>("01ff", {0x01, 0xff});
      value<Bytes>("all_nibbles", {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef});
      value<Bytes>("zero", {0});
      value<Bytes4>("zeros", {0, 0, 0, 0});
      value<Bytes4>("deadbeef", {0xde, 0xad, 0xbe, 0xef});
      value<Vec<Bytes>>("empty", {});
      value<Vec<Bytes>>("mixed", {{}, {0xff}, {1, 2, 3}});
   }

   void emit_containers()
   {
      value<Vec<u64>>("empty", {});
      value<Vec<u64>>("some", {0, 1, std::numeric_limits<u64>::max()});
      value<Vec<u32>>("empty", {});
      value<Vec<u32>>("some", {1, 2, 3});
      value<Vec<Opt<i64>>>("mixed", {std::nullopt, i64{-5}, std::numeric_limits<i64>::min()});
      value<std::array<u16, 3>>("some", {{10, 20, 65535}});
      value<Opt<u64>>("none", std::nullopt);
      value<Opt<u64>>("some", u64{12345678901234567890ull});
      value<Opt<Text>>("none", std::nullopt);
      value<Opt<Text>>("some", text("x"));
      value<Opt<Text>>("some_empty", text(""));
      value<Opt<Bytes>>("none", std::nullopt);
      value<Opt<Bytes>>("some", Bytes{0xab});
      value<Opt<Bytes>>("some_empty", Bytes{});

      value<Tuple0>("unit", {});
      value<Tuple3>("some", {7, text("t"), 1ull << 60});
      value<TupleN>("nested", {{255, -9}, {}, {{}, {0x0a, 0x0b}}});
      value<TupleN>("nested_empty", {{0, 0}, {}, {}});
   }

   void emit_records()
   {
      value<Inner>("small", inner1);
      value<Inner>("big", inner_big);
      value<Outer>("full",
                   Outer{inner1, {inner_big, inner1}, inner1, {1, 2, 255}, std::string("n"), 9, 0.5,
                         {4000000000u, -1}});
      value<Outer>("sparse", Outer{{}, {}, std::nullopt, {}, std::nullopt, 0, 0, {0, 0}});
      value<Overrides>("zero", {});
      value<Overrides>("mixed", Overrides{1, 2, 3, {4, 5}, i64{-6}, -7});
      value<Overrides>("large",
                       Overrides{std::numeric_limits<u64>::max(), std::numeric_limits<u64>::max(),
                                 4294967295u,
                                 {std::numeric_limits<u64>::max(), 0},
                                 std::numeric_limits<i64>::min(), std::numeric_limits<i64>::min()});
      value<TypeLevel>("zero", {});
      value<TypeLevel>("large", TypeLevel{std::numeric_limits<u64>::max(),
                                          std::numeric_limits<i64>::min(), 4294967295u});
      value<AllOptional>("none", {});
      value<AllOptional>("some", AllOptional{u32{5}, text("b")});

      value<Choice>("uint32", Choice{std::in_place_index<0>, 42u});
      value<Choice>("uint64", Choice{std::in_place_index<1>, std::numeric_limits<u64>::max()});
      value<Choice>("string", Choice{std::in_place_index<2>, "text \"q\""});
      value<Choice>("record", Choice{std::in_place_index<3>, inner_big});
      value<Choice>("bytes", Choice{std::in_place_index<4>, Bytes{0xca, 0xfe}});
      value<Holder>("several", Holder{Choice{std::in_place_index<3>, inner1},
                                      {Choice{std::in_place_index<0>, 1u},
                                       Choice{std::in_place_index<1>, u64{2}},
                                       Choice{std::in_place_index<2>, ""},
                                       Choice{std::in_place_index<4>, Bytes{}}}});

      value<Version>("zero", Version{0, 0});
      value<Version>("some", Version{12, 345});
      value<Release>("some", Release{{1, 2}, text("first"), {{0, 9}, {1, 0}}});

      value<Tree>("leaf", tree_value(0));
      value<Tree>("three_levels", tree_value(3));
      // Deepest tree within kMaxValidationDepth = 64 nested containers.
      value<Tree>("depth_limit_31_levels", tree_value(31));
   }

   void emit_accepted()
   {
      // Bare numbers for wide integers under ijson, within the safe range.
      accept<u64>("ijson", "bare_number_small", "42", "\"42\"");
      accept<u64>("ijson", "bare_number_zero", "0", "\"0\"");
      accept<u64>("ijson", "bare_number_safe_max", "9007199254740991", "\"9007199254740991\"");
      accept<i64>("ijson", "bare_number_safe_min", "-9007199254740991", "\"-9007199254740991\"");
      accept<Inner>("ijson", "bare_number_member", R"({"id":7,"label":"seven","flag":true})",
                    R"({"id":"7","label":"seven","flag":true})");
      accept<Vec<u64>>("ijson", "mixed_forms", R"([1,"2",3])", R"(["1","2","3"])");

      // Whitespace between tokens.
      accept_both<Vec<u32>>("whitespace", " [ 1 ,\t2 ,\n3 ]\r\n", "[1,2,3]");
      accept_both<AllOptional>("whitespace", " { \"a\" : 5 , \"b\" : \"x\" } ",
                               R"({"a":5,"b":"x"})");
      accept_both<Tuple0>("whitespace", " [ ] ", "[]");

      // Hex of either case.
      accept<Bytes>("ijson", "lowercase_hex", "\"01ff\"", "\"01FF\"");
      accept<Bytes>("ijson", "mixed_case_hex", "\"aBcDeF\"", "\"ABCDEF\"");
      accept<Bytes4>("ijson", "lowercase_hex", "\"deadbeef\"", "\"DEADBEEF\"");

      // String escapes.
      accept_both<Text>("unicode_escapes", "\"" + u("0041") + "\\/" + u("00e9") + "" + u("2603") + "\"", "\"A/\xC3\xA9\xE2\x98\x83\"");
      accept_both<Text>("surrogate_pair", "\"" + u("d83d") + "" + u("de00") + "\"", "\"\xF0\x9F\x98\x80\"");
      accept_both<Text>("surrogate_pair_uppercase", "\"" + u("D83D") + "" + u("DE00") + "\"", "\"\xF0\x9F\x98\x80\"");
      accept_both<Text>("short_escapes", R"("\b\f\n\r\t\"\\")", "\"" + u("0008") + "" + u("000c") + "\\n\\r\\t\\\"\\\\\"");
      accept_both<Text>("escaped_nul", "\"" + u("0000") + "\"", "\"" + u("0000") + "\"");
      accept_both<std::string>("unicode_escapes", "\"" + u("0041") + "" + u("00e9") + "\"", "\"A\xC3\xA9\"");
      // A noncharacter is JSON but not I-JSON.
      accept<Text>("json", "noncharacter", "\"\xEF\xBF\xBF\"", "\"\xEF\xBF\xBF\"");
      accept<Text>("json", "noncharacter_escape", "\"" + u("ffff") + "\"", "\"\xEF\xBF\xBF\"");

      // Records: any member order, unknown keys skipped, missing optionals.
      accept_both<AllOptional>("member_order", R"({"b":"x","a":5})", R"({"a":5,"b":"x"})");
      accept_both<AllOptional>("missing_optionals", "{}", R"({"a":null,"b":null})");
      accept_both<AllOptional>("missing_one_optional", R"({"b":"x"})", R"({"a":null,"b":"x"})");
      accept_both<AllOptional>("unknown_key_skipped",
                               R"({"a":5,"zzz":{"deep":[1,2.5e3,"s",null,true,false,{}]},"b":"x"})",
                               R"({"a":5,"b":"x"})");
      accept_both<AllOptional>("escaped_key", "{\"" + u("0061") + "\":5}", R"({"a":5,"b":null})");
      accept_both<Tree>("unknown_key_only_extra", "{\"children\":[],\"extra\":\"" + u("d83d") + "" + u("de00") + "\"}", R"({"children":[]})");
      // RFC 8259 allows repeated keys: psio::json keeps the last.
      accept<AllOptional>("json", "duplicate_key_last_wins", R"({"a":1,"a":2})",
                          R"({"a":2,"b":null})");
      accept<AllOptional>("json", "duplicate_unknown_key", R"({"z":1,"z":2})",
                          R"({"a":null,"b":null})");
      // Unknown key nested to the depth limit: object + 63 arrays.
      accept_both<AllOptional>("unknown_key_depth_63",
                               "{\"x\":" + repeat("[", 63) + repeat("]", 63) + "}",
                               R"({"a":null,"b":null})");

      // Floats: any JSON number.
      accept_both<double>("fraction_zero", "1.0", "1");
      accept_both<double>("exponent", "1e2", "100");
      accept_both<double>("exponent_upper_plus", "1.5E+3", "1500");
      accept_both<double>("minus_zero", "-0.0", "-0");
      accept_both<double>("integer_token", "5", "5");
      accept_both<double>("underflow_to_zero", "1e-400", "0");
      accept_both<double>("long_mantissa", "0.1000000000000000055511151231257827", "0.1");
      accept_both<float>("rounds_to_float", "0.1", "0.1");
      accept_both<float>("exponent", "2.5e1", "25");
   }

   void emit_rejected()
   {
      // ── Syntax ─────────────────────────────────────────────────────────
      reject_both<u32>("empty", "");
      reject_both<u32>("only_whitespace", "  ");
      reject_both<u32>("trailing_garbage", "1 x");
      reject_both<u32>("two_values", "1 2");
      reject_both<Vec<u32>>("trailing_comma", "[1,]");
      reject_both<Vec<u32>>("leading_comma", "[,1]");
      reject_both<Vec<u32>>("missing_comma", "[1 2]");
      reject_both<Vec<u32>>("unterminated", "[1,2");
      reject_both<AllOptional>("trailing_comma", R"({"a":1,})");
      reject_both<AllOptional>("missing_colon", R"({"a" 1})");
      reject_both<AllOptional>("unquoted_key", R"({a:1})");
      reject_both<AllOptional>("single_quotes", "{'a':1}");
      reject_both<AllOptional>("unterminated", R"({"a":1)");
      reject_both<AllOptional>("unknown_key_bad_value", R"({"zzz":[1,})");
      reject_both<AllOptional>("unknown_key_bad_number", R"({"zzz":01})");
      reject_both<bool>("capitalized", "True");
      reject_both<bool>("truncated", "tru");
      reject_both<bool>("number", "1");
      reject_both<Opt<u64>>("capitalized_null", "Null");
      reject_both<Opt<u64>>("truncated_null", "nul");

      // ── Wrong type ─────────────────────────────────────────────────────
      reject_both<u32>("string_for_u32", "\"1\"");
      reject_both<u32>("bool_for_u32", "true");
      reject_both<u32>("null_for_u32", "null");
      reject_both<u32>("array_for_u32", "[1]");
      reject_both<Text>("number_for_string", "1");
      reject_both<Text>("null_for_string", "null");
      reject_both<bool>("string_for_bool", "\"true\"");
      reject_both<Vec<u32>>("object_for_list", "{}");
      reject_both<Vec<u32>>("string_element", "[1,\"2\"]");
      reject_both<Inner>("array_for_record", "[]");
      reject_both<Inner>("member_wrong_type", R"({"id":"7","label":7,"flag":true})");
      reject_both<Inner>("null_for_member", R"({"id":"7","label":null,"flag":true})");
      reject_both<Inner>("missing_required_member", R"({"id":"7","label":"x"})");
      reject_both<Inner>("empty_object", "{}");
      reject<u64>("json", "string_for_u64", "\"1\"");
      reject<i64>("json", "string_for_i64", "\"-1\"");
      reject<Bytes>("json", "hex_string_for_bytes", "\"01FF\"");
      reject<Bytes>("ijson", "number_array_for_bytes", "[1,255]");
      reject<Bytes>("ijson", "number_for_bytes", "1");
      reject_both<double>("string_for_f64", "\"1.5\"");

      // ── Integer ranges and number forms ────────────────────────────────
      reject_both<u8>("256", "256");
      reject_both<u8>("minus_one", "-1");
      reject_both<i8>("128", "128");
      reject_both<i8>("minus_129", "-129");
      reject_both<u16>("65536", "65536");
      reject_both<u32>("4294967296", "4294967296");
      reject_both<i32>("2147483648", "2147483648");
      reject_both<u32>("fraction", "1.0");
      reject_both<u32>("exponent", "1e2");
      reject_both<u32>("leading_zero", "01");
      reject_both<u32>("plus_sign", "+1");
      reject_both<u32>("minus_zero", "-0");
      reject_both<u32>("hex_literal", "0x10");
      reject_both<i32>("lone_minus", "-");
      reject<u64>("json", "2^64", "18446744073709551616");
      reject<u64>("json", "minus_one", "-1");
      reject<i64>("json", "2^63", "9223372036854775808");
      reject<i64>("json", "below_min", "-9223372036854775809");

      // ── I-JSON integer strings ─────────────────────────────────────────
      reject<u64>("ijson", "string_2^64", "\"18446744073709551616\"");
      reject<u64>("ijson", "string_huge", "\"99999999999999999999999999999999999999\"");
      reject<u64>("ijson", "string_minus_one", "\"-1\"");
      reject<i64>("ijson", "string_2^63", "\"9223372036854775808\"");
      reject<i64>("ijson", "string_below_min", "\"-9223372036854775809\"");
      reject<u64>("ijson", "string_leading_zero", "\"01\"");
      reject<u64>("ijson", "string_double_zero", "\"00\"");
      reject<u64>("ijson", "string_plus_sign", "\"+1\"");
      reject<u64>("ijson", "string_leading_space", "\" 1\"");
      reject<u64>("ijson", "string_trailing_space", "\"1 \"");
      reject<u64>("ijson", "string_inner_space", "\"1 2\"");
      reject<u64>("ijson", "string_empty", "\"\"");
      reject<i64>("ijson", "string_lone_minus", "\"-\"");
      reject<i64>("ijson", "string_minus_zero", "\"-0\"");
      reject<i64>("ijson", "string_minus_leading_zero", "\"-01\"");
      reject<u64>("ijson", "string_fraction", "\"1.0\"");
      reject<u64>("ijson", "string_exponent", "\"1e3\"");
      reject<u64>("ijson", "string_hex", "\"0x10\"");
      reject<u64>("ijson", "string_escaped_digit", "\"" + u("0031") + "\"");
      reject<u64>("ijson", "string_newline", "\"1\\n\"");
      reject<u64>("ijson", "string_unterminated", "\"1");
      // A bare number is exact only up to 2^53 - 1.
      reject<u64>("ijson", "bare_2^53", "9007199254740992");
      reject<u64>("ijson", "bare_2^53+1", "9007199254740993");
      reject<u64>("ijson", "bare_max", "18446744073709551615");
      reject<i64>("ijson", "bare_-2^53", "-9007199254740992");
      reject<i64>("ijson", "bare_min", "-9223372036854775808");
      reject<u64>("ijson", "bare_fraction", "1.0");
      reject<u64>("ijson", "bare_leading_zero", "01");
      reject<Opt<u64>>("ijson", "bare_2^53_in_optional", "9007199254740992");
      reject<Vec<u64>>("ijson", "bare_2^53_in_list", "[1,9007199254740992]");
      reject<Inner>("ijson", "bare_2^53_member",
                    R"({"id":9007199254740992,"label":"x","flag":true})");

      // ── Forced forms accept exactly that form ──────────────────────────
      const std::string ov_json  = R"({"a":"1","b":2,"c":"3","d":[4,5],"e":"-6","f":-7})";
      const std::string ov_ijson = R"({"a":"1","b":2,"c":"3","d":[4,5],"e":"-6","f":"-7"})";
      if (psio::encode(psio::json{}, Overrides{1, 2, 3, {4, 5}, i64{-6}, -7}) != ov_json ||
          psio::encode(psio::ijson{}, Overrides{1, 2, 3, {4, 5}, i64{-6}, -7}) != ov_ijson)
         die("Overrides texts are stale");
      reject<Overrides>("json", "forced_string_given_number",
                        R"({"a":1,"b":2,"c":"3","d":[4,5],"e":"-6","f":-7})");
      reject<Overrides>("json", "forced_string_u32_given_number",
                        R"({"a":"1","b":2,"c":3,"d":[4,5],"e":"-6","f":-7})");
      reject<Overrides>("json", "forced_string_optional_given_number",
                        R"({"a":"1","b":2,"c":"3","d":[4,5],"e":-6,"f":-7})");
      reject<Overrides>("json", "natural_number_given_string",
                        R"({"a":"1","b":"2","c":"3","d":[4,5],"e":"-6","f":-7})");
      reject<Overrides>("ijson", "forced_number_given_string",
                        R"({"a":"1","b":"2","c":"3","d":[4,5],"e":"-6","f":"-7"})");
      reject<Overrides>("ijson", "forced_number_list_given_string",
                        R"({"a":"1","b":2,"c":"3","d":["4",5],"e":"-6","f":"-7"})");
      reject<Overrides>("ijson", "forced_string_u32_given_number",
                        R"({"a":"1","b":2,"c":3,"d":[4,5],"e":"-6","f":"-7"})");
      reject<Overrides>("ijson", "forced_string_out_of_range",
                        R"({"a":"1","b":2,"c":"4294967296","d":[4,5],"e":"-6","f":"-7"})");
      reject<Overrides>("ijson", "forced_number_out_of_range",
                        R"({"a":"1","b":18446744073709551616,"c":"3","d":[4,5],"e":"-6","f":"-7"})");
      reject<TypeLevel>("ijson", "type_level_number_given_string", R"({"x":"1","y":2,"z":3})");
      reject<TypeLevel>("json", "type_level_string_given_number", R"({"x":1,"y":"2","z":"3"})");

      // ── Floats ─────────────────────────────────────────────────────────
      reject_both<double>("nan", "NaN");
      reject_both<double>("nan_lower", "nan");
      reject_both<double>("infinity", "Infinity");
      reject_both<double>("minus_inf", "-inf");
      reject_both<double>("overflow", "1e999");
      reject_both<float>("overflow", "1e39");
      reject_both<double>("leading_dot", ".5");
      reject_both<double>("trailing_dot", "1.");
      reject_both<double>("bare_exponent", "1e");
      reject_both<double>("leading_zero", "01.5");
      reject_both<double>("plus_sign", "+1.5");
      reject_both<double>("hex_float", "0x1p3");
      reject_both<double>("null", "null");

      // ── Strings ────────────────────────────────────────────────────────
      reject_both<Text>("unterminated", "\"abc");
      reject_both<Text>("raw_newline", "\"a\nb\"");
      reject_both<Text>("raw_nul", std::string("\"a\0b\"", 5));
      reject_both<Text>("bad_escape", R"("\x41")");
      reject_both<Text>("short_unicode_escape", "\"\\u" "00\"");
      reject_both<Text>("bad_unicode_escape", "\"\\u" "00zz\"");
      reject_both<Text>("lone_high_surrogate", "\"" + u("d83d") + "\"");
      reject_both<Text>("lone_low_surrogate", "\"" + u("de00") + "\"");
      reject_both<Text>("high_surrogate_then_text", "\"" + u("d83d") + "x\"");
      reject_both<Text>("high_surrogate_then_high", "\"" + u("d83d") + "" + u("d83d") + "\"");
      reject_both<Text>("invalid_utf8_ff", "\"\xff\"");
      reject_both<Text>("invalid_utf8_truncated", "\"\xE2\x98\"");
      reject_both<Text>("invalid_utf8_overlong", "\"\xC0\x80\"");
      reject_both<Text>("invalid_utf8_surrogate", "\"\xED\xA0\x80\"");
      reject_both<Text>("invalid_utf8_above_max", "\"\xF4\x90\x80\x80\"");
      reject_both<std::string>("invalid_utf8_ff", "\"\xff\"");
      reject_both<std::string>("lone_high_surrogate", "\"" + u("d83d") + "\"");
      reject<Text>("ijson", "noncharacter_ffff", "\"\xEF\xBF\xBF\"");
      reject<Text>("ijson", "noncharacter_escape", "\"" + u("fffe") + "\"");
      reject<Text>("ijson", "noncharacter_fdd0", "\"\xEF\xB7\x90\"");
      reject<Text>("ijson", "noncharacter_plane_16", "\"\xF4\x8F\xBF\xBF\"");
      reject<AllOptional>("ijson", "noncharacter_in_unknown_value", "{\"z\":\"" + u("ffff") + "\"}");
      reject_both<AllOptional>("invalid_utf8_key", "{\"\xff\":1}");

      // ── Hex ────────────────────────────────────────────────────────────
      reject<Bytes>("ijson", "odd_length", "\"0\"");
      reject<Bytes>("ijson", "odd_length_three", "\"01F\"");
      reject<Bytes>("ijson", "non_hex", "\"0G\"");
      reject<Bytes>("ijson", "prefixed", "\"0x01\"");
      reject<Bytes>("ijson", "space", "\"01 FF\"");
      reject<Bytes>("ijson", "escaped", "\"" + u("0030") + "" + u("0031") + "\"");
      reject<Bytes>("ijson", "unterminated", "\"01");
      reject<Bytes>("ijson", "null", "null");
      reject<Bytes4>("ijson", "too_short", "\"DEADBE\"");
      reject<Bytes4>("ijson", "too_long", "\"DEADBEEF00\"");
      reject<Bytes4>("ijson", "odd_length", "\"DEADBEE\"");
      reject<Bytes4>("json", "too_short", "[1,2,3]");
      reject<Bytes4>("json", "too_long", "[1,2,3,4,5]");
      reject<Bytes>("json", "element_out_of_range", "[256]");
      reject<Bytes>("json", "negative_element", "[-1]");

      // ── Arrays and tuples: exact arity ─────────────────────────────────
      reject_both<std::array<u16, 3>>("too_short", "[1,2]");
      reject_both<std::array<u16, 3>>("too_long", "[1,2,3,4]");
      reject_both<std::array<u16, 3>>("empty", "[]");
      reject_both<Tuple0>("unit_with_element", "[1]");
      reject_both<Tuple0>("unit_as_null", "null");
      reject_both<Tuple0>("unit_as_object", "{}");
      reject<Tuple3>("json", "too_short", R"([7,"t"])");
      reject<Tuple3>("ijson", "too_short", R"([7,"t"])");
      reject<Tuple3>("json", "too_long", R"([7,"t",1,2])");
      reject<Tuple3>("ijson", "too_long", R"([7,"t","1","2"])");
      reject_both<Tuple3>("empty", "[]");
      reject_both<Tuple3>("object", R"({"0":7})");
      reject<Tuple3>("ijson", "wrong_element_type", R"([7,8,"1"])");
      reject<TupleN>("ijson", "inner_too_short", R"([[255],[],[]])");
      reject<TupleN>("ijson", "inner_unit_not_empty", R"([[255,"-9"],[0],[]])");
      reject<Outer>("ijson", "member_tuple_too_long",
                    R"({"inner":{"id":"0","label":"","flag":false},"items":[],"maybe":null,"blob":"","note":null,"count":0,"ratio":0,"pair":[0,"0",0]})");

      // ── Variants ───────────────────────────────────────────────────────
      reject<Choice>("ijson", "unknown_name", R"({"int32":1})");
      reject<Choice>("ijson", "unknown_name_case", R"({"Uint32":1})");
      reject<Choice>("ijson", "empty_name", R"({"":1})");
      reject<Choice>("ijson", "two_keys", R"({"uint32":1,"uint64":"2"})");
      reject<Choice>("ijson", "same_key_twice", R"({"uint32":1,"uint32":1})");
      reject<Choice>("ijson", "no_key", "{}");
      reject<Choice>("ijson", "index_form", "[0,1]");
      reject<Choice>("ijson", "bare_value", "1");
      reject<Choice>("ijson", "null", "null");
      reject<Choice>("ijson", "wrong_payload_type", R"({"uint32":"1"})");
      reject<Choice>("ijson", "payload_out_of_range", R"({"uint32":4294967296})");
      reject<Choice>("ijson", "bytes_payload_odd_hex", R"({"uint8[]":"ABC"})");
      reject<Choice>("ijson", "record_payload_missing_member", R"({"Inner":{"id":"1"}})");
      reject<Choice>("json", "named_form", R"({"uint32":1})");
      reject<Choice>("json", "index_out_of_range", "[5,1]");
      reject<Choice>("json", "negative_index", "[-1,1]");
      reject<Choice>("json", "index_only", "[0]");
      reject<Choice>("json", "extra_element", "[0,1,2]");
      reject<Choice>("json", "string_index", R"(["0",1])");
      reject<Choice>("json", "wrong_payload_type", R"([0,"1"])");

      // ── Duplicate keys (RFC 7493) ──────────────────────────────────────
      reject<AllOptional>("ijson", "duplicate_key", R"({"a":1,"a":2})");
      reject<AllOptional>("ijson", "duplicate_key_same_value", R"({"a":1,"a":1})");
      reject<AllOptional>("ijson", "duplicate_key_escaped", "{\"a\":1,\"" + u("0061") + "\":1}");
      reject<AllOptional>("ijson", "duplicate_unknown_key", R"({"z":1,"z":2})");
      reject<AllOptional>("ijson", "duplicate_unknown_key_escaped", "{\"z\":1,\"" + u("007a") + "\":2}");
      reject<AllOptional>("ijson", "duplicate_key_inside_unknown_value",
                          R"({"z":{"k":1,"k":2}})");
      reject<AllOptional>("ijson", "duplicate_key_deep_inside_unknown_value",
                          R"({"z":[1,{"q":[{"k":1,"j":0,"k":2}]}]})");
      reject<Inner>("ijson", "duplicate_member",
                    R"({"id":"7","label":"x","flag":true,"id":"7"})");
      reject<Holder>("ijson", "duplicate_key_in_nested_record",
                     R"({"choice":{"Inner":{"id":"1","id":"1","label":"","flag":false}},"choices":[]})");

      // ── Custom text form ───────────────────────────────────────────────
      reject_both<Version>("not_a_string", "1.2");
      reject_both<Version>("missing_minor", "\"1\"");
      reject_both<Version>("out_of_range", "\"1.65536\"");
      reject_both<Version>("extra_part", "\"1.2.3\"");
      reject_both<Version>("object", R"({"major":1,"minor":2})");
      reject_both<Release>("bad_member", R"({"version":"x","name":"n","history":[]})");

      // ── Depth ──────────────────────────────────────────────────────────
      reject_both<Tree>("depth_32_levels", tree_text(32));
      reject_both<Tree>("depth_200_levels", tree_text(200));
      reject_both<AllOptional>("unknown_key_depth_64",
                               "{\"x\":" + repeat("[", 64) + repeat("]", 64) + "}");
      reject_both<AllOptional>("unknown_key_depth_bomb_arrays", "{\"x\":" + repeat("[", 5000));
      reject_both<AllOptional>("unknown_key_depth_bomb_objects", repeat("{\"x\":", 5000));
      reject_both<Vec<u32>>("array_bomb", repeat("[", 5000));
   }

   std::string quoted(const std::string& s) { return psio::encode(psio::json{}, s); }

   std::string render()
   {
      std::string out = "{\"version\":1,\"vectors\":[\n";
      for (std::size_t i = 0; i < rows.size(); ++i)
      {
         const Row& r = rows[i];
         out += "{\"name\":" + quoted(r.name) + ",\"type\":" + quoted(r.type) +
                ",\"kind\":" + quoted(r.kind);
         auto field = [&](const char* key, const std::optional<std::string>& v) {
            if (v) out += std::string(",\"") + key + "\":" + quoted(*v);
         };
         field("format", r.format);
         field("json", r.json);
         field("ijson", r.ijson);
         field("text", r.text);
         field("text_hex", r.text_hex);
         field("canonical", r.canonical);
         out += i + 1 < rows.size() ? "},\n" : "}\n";
      }
      out += "]}\n";
      return out;
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   if (!check && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json>\n", argv[0]);
      return 2;
   }
   emit_scalars();
   emit_strings_and_bytes();
   emit_containers();
   emit_records();
   emit_accepted();
   emit_rejected();

   const std::string text = render();
   // The fixture is itself read back with the codec under test.
   Fixture parsed;
   if (!psio::decode_json_checked(psio::json{}, std::span<const char>{text}, parsed).ok() ||
       parsed.vectors.size() != rows.size())
      die("the rendered fixture does not read back");

   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "json conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended format change\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   std::printf("wrote %zu vectors to %s\n", rows.size(), argv[1]);
   return out ? 0 : 1;
}
