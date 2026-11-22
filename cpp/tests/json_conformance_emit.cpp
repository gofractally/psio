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

#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

   std::string lower_hex(std::span<const char> bytes)
   {
      static const char digits[] = "0123456789abcdef";
      std::string       out;
      for (char c : bytes)
      {
         out.push_back(digits[static_cast<unsigned char>(c) >> 4]);
         out.push_back(digits[static_cast<unsigned char>(c) & 15]);
      }
      return out;
   }

   // One Schema holding every type under its fixture id.
   const psio::Schema& fixture_schema()
   {
      static const psio::Schema schema = [] {
         psio::SchemaBuilder builder;
         for_each_type([&]<typename T>(std::type_identity<T>) {
            if constexpr (has_ir<T>) builder.insert<T>(type_id<T>());
         });
         return std::move(builder).build();
      }();
      return schema;
   }
   // The fixture's custom text forms, each taken from the type's adapter.
   const psio::pssz_ir::CustomHandlers& fixture_handlers()
   {
      static const psio::pssz_ir::CustomHandlers handlers = [] {
         psio::pssz_ir::CustomHandlers h;
         h.add<Version>();
         h.add<Account>();
         custom_conf::for_each_custom(
            [&]<typename T>(std::type_identity<T>) { h.template add<T>(); });
         return h;
      }();
      return handlers;
   }

   // A value row of a type holding a custom text form: the text of the
   // schema path without a handler, and the checks that the schema path
   // with the handlers is the typed codec.
   template <typename T>
   void custom_texts(Row& row, const T& v)
   {
      namespace ir = psio::pssz_ir;
      const auto&                       schema = fixture_schema();
      const psio::schema_types::AnyType root   = psio::schema_types::Type{type_id<T>()};
      const std::vector<char>           bytes  = psio::encode(psio::pssz{}, v);
      auto text_of = [&](auto fmt, const ir::CustomHandlers* h) {
         std::string out;
         if (!ir::pssz_to_json(schema, root, fmt, std::span<const char>{bytes}, out, nullptr, h)
                 .ok())
            die(row.name + ": schema path cannot write the value");
         return out;
      };
      auto bytes_of = [&](auto fmt, const std::string& text, const ir::CustomHandlers* h) {
         std::vector<char> out;
         if (!ir::json_to_pssz(schema, root, fmt, std::span<const char>{text}, out, nullptr,
                               psio::json_parser::automatic, h)
                 .ok())
            die(row.name + ": schema path cannot read " + text);
         return out;
      };
      const auto* with = &fixture_handlers();
      if (text_of(psio::json{}, with) != *row.json || text_of(psio::ijson{}, with) != *row.ijson)
         die(row.name + ": schema path with handlers differs from the typed text");
      if (bytes_of(psio::json{}, *row.json, with) != bytes ||
          bytes_of(psio::ijson{}, *row.ijson, with) != bytes)
         die(row.name + ": schema path with handlers reads different bytes");
      // A type with an invariant and no text form of its own: the schema
      // path is checked against the typed codec, and there is no fallback
      // text to record.
      if constexpr (!has_custom<T>)
         return;
      row.plain_json  = text_of(psio::json{}, nullptr);
      row.plain_ijson = text_of(psio::ijson{}, nullptr);
      if (*row.plain_json == *row.json)
         die(row.name + ": the fallback text does not differ from the custom text");
      if (bytes_of(psio::json{}, *row.plain_json, nullptr) != bytes ||
          bytes_of(psio::ijson{}, *row.plain_ijson, nullptr) != bytes)
         die(row.name + ": the fallback text does not read back");
      // With require_all, a missing handler is an error, not the fallback.
      ir::CustomHandlers strict;
      strict.require_all = true;
      std::string        out;
      if (ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{bytes}, out, nullptr,
                           &strict)
             .ok())
         die(row.name + ": require_all accepted a missing handler");
   }

   template <typename T>
   void push(Row row)
   {
      row.type = type_id<T>();
      row.name = row.kind + "/" + row.type + "/" + row.name;
      if (row.format) row.name += "/" + *row.format;
      if (!names.insert(row.name).second) die("duplicate row " + row.name);
      if (auto e = check_row<T>(row); !e.empty()) die(row.name + ": " + e);
      // The typed psio::pssz bytes of the row's value.
      if constexpr (has_pssz<T>)
         if (row.kind != "reject" && row.kind != "invariant")
         {
            T                 v{};
            const std::string text = row.kind == "value" ? *row.json : input_text(row);
            const bool ok = row.kind == "value" || row.format == "json"
                               ? psio::decode_json_checked(psio::json{},
                                                           std::span<const char>{text}, v)
                                    .ok()
                               : psio::decode_json_checked(psio::ijson{},
                                                           std::span<const char>{text}, v)
                                    .ok();
            if (!ok) die(row.name + ": the row's text does not decode");
            std::vector<char> bytes;
            if (!psio::encode_pssz_checked(psio::pssz{}, v, bytes).ok())
               die(row.name + ": the value has no pssz encoding");
            if (!psio::validate<T>(psio::pssz{}, std::span<const char>{bytes}).ok())
               die(row.name + ": the pssz encoding does not validate");
            row.pssz = lower_hex(bytes);
         }
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
      if constexpr (has_custom<T> || has_invariant<T>)
         custom_texts<T>(row, v);
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

   // A value that violates an invariant, given as the text of its wire type
   // (`plain`: what a schema-driven codec with no handler reads) and, when
   // the typed text differs from that — a custom text form on the way — the
   // typed text in each format. Two rows: one per text format.
   template <typename T>
   void invariant(const std::string& name, const std::string& reason, const std::string& plain,
                  const std::string& typed_json = "", const std::string& typed_ijson = "")
   {
      namespace ir = psio::pssz_ir;
      const auto&                       schema = fixture_schema();
      const psio::schema_types::AnyType root   = psio::schema_types::Type{type_id<T>()};
      const auto*                       with   = &fixture_handlers();
      // Without a handler the value is its wire type: read, written, valid.
      std::vector<char> bytes;
      if (!ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{plain}, bytes).ok())
         die(name + ": the schema path with no handler does not read " + plain);
      if (!ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok())
         die(name + ": the bytes are not structurally valid");
      auto text_of = [&](auto fmt) {
         std::string out;
         if (!ir::pssz_to_json(schema, root, fmt, std::span<const char>{bytes}, out).ok())
            die(name + ": the schema path with no handler cannot write the value");
         std::vector<char> back;
         if (!ir::json_to_pssz(schema, root, fmt, std::span<const char>{out}, back).ok() ||
             back != bytes)
            die(name + ": the fallback text does not read back");
         return out;
      };
      const std::string plain_json  = text_of(psio::json{});
      const std::string plain_ijson = text_of(psio::ijson{});
      // With the handlers every schema-driven entry point reports the
      // invariant; a strict registry with no handler refuses the type.
      auto must_fail = [&](psio::codec_status st, const char* what) {
         if (st.ok() || st.error().what != reason)
            die(name + ": " + what + " does not report '" + reason + "'" +
                (st.ok() ? "" : " but '" + std::string(st.error().what) + "'"));
      };
      must_fail(ir::validate_pssz(schema, root, std::span<const char>{bytes}, nullptr, with),
                "validate_pssz with handlers");
      {
         ir::Value value;
         bool      unknown = false;
         must_fail(ir::decode_auto(schema, root, std::span<const char>{bytes}, value, unknown,
                                   with),
                   "decode_auto with handlers");
         if (!ir::decode_auto(schema, root, std::span<const char>{bytes}, value, unknown).ok())
            die(name + ": decode_auto with no handler");
         std::vector<char> out;
         must_fail(ir::encode_auto(schema, root, value, out, with), "encode_auto with handlers");
         std::string text;
         must_fail(ir::value_to_json(schema, root, psio::json{}, value, text, with),
                   "value_to_json with handlers");
         must_fail(ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{bytes},
                                    text, nullptr, with),
                   "pssz_to_json with handlers");
         ir::CustomHandlers strict;
         strict.require_all = true;
         auto st = ir::validate_pssz(schema, root, std::span<const char>{bytes}, nullptr, &strict);
         if (st.ok() || st.error().what != "pssz IR: no handler registered for a custom type")
            die(name + ": a strict registry accepted a type it has no handler for");
      }
      auto row_for = [&](const char* format, const std::string& text, auto fmt) {
         std::vector<char> out;
         must_fail(ir::json_to_pssz(schema, root, fmt, std::span<const char>{text}, out, nullptr,
                                    psio::json_parser::automatic, with),
                   "json_to_pssz with handlers");
         ir::Value value;
         must_fail(ir::json_to_value(schema, root, fmt, std::span<const char>{text}, value,
                                     nullptr, psio::json_parser::automatic, with),
                   "json_to_value with handlers");
         Row row;
         row.name        = name;
         row.kind        = "invariant";
         row.format      = format;
         row.pssz        = lower_hex(bytes);
         row.plain_json  = plain_json;
         row.plain_ijson = plain_ijson;
         row.reason      = reason;
         set_text(row, text);
         push<T>(std::move(row));
      };
      row_for("json", typed_json.empty() ? plain_json : typed_json, psio::json{});
      row_for("ijson", typed_ijson.empty() ? plain_ijson : typed_ijson, psio::ijson{});
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
      value<i64>("safe_max_2^53-1", (i64{1} << 53) - 1);
      value<i64>("safe_min_-(2^53-1)", -((i64{1} << 53) - 1));
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
      // Exactly halfway between two shortest candidates: the even one wins.
      value<float>("tie_to_even", -271183.625f);
      value<float>("tie_to_even_2", 271183.375f);
      value<double>("tie_to_even", 4503599627370497.5);
      value<double>("tie_to_even_2", 9007199254740993.0 * 2);
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

   // Custom text forms over a 64-bit integer (Account) and over a record
   // (Version), in every position.
   void emit_custom()
   {
      auto account = [](std::string_view name) {
         Account a{};
         if (!AccountText::parse(std::span<const char>{name.data(), name.size()}, a))
            die("bad account literal");
         return a;
      };
      value<Account>("empty", account(""));
      value<Account>("a", account("a"));
      value<Account>("alice", account("alice"));
      value<Account>("dotted", account("bob.token"));
      value<Account>("digits", account("a1b2c3d4e5"));
      value<Account>("twelve", account("zzzzzzzzzzzz"));
      value<Account>("leading_dot", account(".a"));
      value<Version>("max", Version{65535, 65535});
      value<Transfer>("zero", Transfer{});
      value<Transfer>("some", Transfer{account("alice"), account("bob"), 18446744073709551615ull,
                                       account("carol"), {account("dave"), account("")},
                                       Version{1, 2}});
      value<Transfer>("no_optionals", Transfer{account("a"), account("b"), 7, std::nullopt,
                                               {account("zzzzzzzzzzzz")}, std::nullopt});

      accept_both<Transfer>("missing_optionals",
                            R"({"from":"a","to":"b","amount":7,"notify":[]})",
                            R"({"from":"a","to":"b","amount":7,"payer":null,"notify":[],"since":null})");
      reject_both<Account>("number", "7");
      reject_both<Account>("upper_case", R"("Alice")");
      reject_both<Account>("thirteen_characters", R"("aaaaaaaaaaaaa")");
      reject_both<Account>("trailing_dot", R"("alice.")");
      reject_both<Account>("bad_digit", R"("a6")");
      reject_both<Account>("null", "null");
      reject_both<Account>("object", R"({"value":7})");
      reject_both<Transfer>("account_as_number",
                            R"({"from":1,"to":"b","amount":7,"payer":null,"notify":[],"since":null})");
      reject_both<Transfer>("bad_account_in_list",
                            R"({"from":"a","to":"b","amount":7,"payer":null,"notify":["B"],"since":null})");
      reject_both<Transfer>("version_as_object",
                            R"({"from":"a","to":"b","amount":7,"payer":null,"notify":[],"since":{"major":1,"minor":2}})");
   }

   // Enums with enumerator names: a listed value is its name, any other
   // its number; the name, the number, the numeric string and "name:number"
   // are all read.
   void emit_enums()
   {
      value<Color>("green", Color::green);
      value<Color>("unnamed_2", static_cast<Color>(2));
      value<Status>("pending", Status::pending);
      value<Status>("active", Status::active);
      value<Status>("closed", Status::closed);
      value<Status>("unnamed_0", static_cast<Status>(0));
      value<Status>("unnamed_min", static_cast<Status>(std::numeric_limits<std::int32_t>::min()));
      value<Status>("unnamed_max", static_cast<Status>(std::numeric_limits<std::int32_t>::max()));

      accept_both<Color>("name", R"("blue")", R"("blue")");
      accept_both<Color>("number_of_a_name", "7", R"("blue")");
      accept_both<Color>("numeric_string_of_a_name", R"("7")", R"("blue")");
      accept_both<Color>("name_and_number", R"("blue:7")", R"("blue")");
      accept_both<Color>("name_and_zero", R"("red:0")", R"("red")");
      accept_both<Color>("numeric_string_unnamed", R"("9")", "9");
      accept_both<Color>("number_unnamed", "9", "9");
      accept_both<Color>("escaped_name", R"("red")", R"("red")");
      accept_both<Status>("negative_name", R"("pending")", R"("pending")");
      accept_both<Status>("negative_number", "-1", R"("pending")");
      accept_both<Status>("negative_numeric_string", R"("-1")", R"("pending")");
      accept_both<Status>("name_and_negative_number", R"("pending:-1")", R"("pending")");
      accept_both<Status>("unnamed_negative", "-5", "-5");

      reject_both<Color>("unknown_name", R"("purple")");
      reject_both<Color>("name_wrong_case", R"("Red")");
      reject_both<Color>("empty_string", R"("")");
      reject_both<Color>("name_and_wrong_number", R"("red:1")");
      reject_both<Color>("name_and_missing_number", R"("red:")");
      reject_both<Color>("missing_name", R"(":0")");
      reject_both<Color>("number_then_name", R"("0:red")");
      reject_both<Color>("unknown_name_and_number", R"("purple:0")");
      reject_both<Color>("name_and_noncanonical_number", R"("red:00")");
      reject_both<Color>("name_and_out_of_range_number", R"("red:256")");
      reject_both<Color>("numeric_string_leading_zero", R"("07")");
      reject_both<Color>("numeric_string_out_of_range", R"("256")");
      reject_both<Color>("numeric_string_negative", R"("-1")");
      reject_both<Color>("numeric_string_plus", R"("+1")");
      reject_both<Color>("name_with_space", R"(" red")");
      reject_both<Color>("two_colons", R"("red:0:0")");
      reject_both<Color>("fraction", "1.0");
      reject_both<Color>("bool", "true");
      reject_both<Color>("null", "null");
      reject_both<Color>("array", R"(["red"])");
      reject_both<Status>("out_of_range", "2147483648");
      reject_both<Status>("minus_zero", "-0");
      reject_both<Status>("name_and_wrong_sign", R"("pending:1")");
   }

   // 128- and 256-bit integers: a bare decimal number in json, a decimal
   // string in ijson. Read from either, and from the older "0x" string.
   void emit_wide_integers()
   {
      using u128 = psio::uint128;
      using i128 = psio::int128;
      using u256 = psio::uint256;
      const u128 u128_max = ~u128{0};
      const i128 i128_max = static_cast<i128>(u128_max >> 1);
      const i128 i128_min = -i128_max - 1;
      u256       u256_max;
      for (auto& limb : u256_max.limb)
         limb = ~std::uint64_t{0};
      u256 two_pow_128;
      two_pow_128.limb[2] = 1;
      u256 mixed;
      mixed.limb[0] = 0x0123456789abcdefull;
      mixed.limb[1] = 0xfedcba9876543210ull;
      mixed.limb[2] = 0x0f1e2d3c4b5a6978ull;
      mixed.limb[3] = 0x8877665544332211ull;

      value<u128>("zero", 0);
      value<u128>("one", 1);
      value<u128>("safe_max", (u128{1} << 53) - 1);
      value<u128>("safe_max_plus_one", u128{1} << 53);
      value<u128>("u64_max", ~std::uint64_t{0});
      value<u128>("2^64", u128{1} << 64);
      value<u128>("1e30", u128{1000000000000000ull} * 1000000000000000ull);
      value<u128>("max", u128_max);
      value<i128>("zero", 0);
      value<i128>("minus_one", -1);
      value<i128>("minus_safe_max", -static_cast<i128>((u128{1} << 53) - 1));
      value<i128>("minus_2^64", -static_cast<i128>(u128{1} << 64));
      value<i128>("max", i128_max);
      value<i128>("min", i128_min);
      value<u256>("zero", u256{});
      value<u256>("one", u256{std::uint64_t{1}});
      value<u256>("u64_max", u256{~std::uint64_t{0}});
      value<u256>("2^128", two_pow_128);
      value<u256>("mixed", mixed);
      value<u256>("max", u256_max);
      value<Wide>("zero", Wide{});
      value<Wide>("some", Wide{u128{1} << 100, i128_min, mixed, u128_max, {0, -1, i128_max},
                               Status::active});
      value<Wide>("small", Wide{7, -7, u256{std::uint64_t{7}}, u128{7}, {7},
                                static_cast<Status>(3)});

      const std::string max128 = "340282366920938463463374607431768211455";
      const std::string max256 =
         "115792089237316195423570985008687907853269984665640564039457584007913129639935";
      // Either form is read in both formats; the writer has one per format.
      accept<u128>("json", "decimal_string", "\"" + max128 + "\"", max128);
      accept<u128>("ijson", "decimal_string_small", R"("7")", R"("7")");
      accept<u128>("ijson", "bare_number_small", "7", R"("7")");
      accept<u128>("ijson", "bare_number_safe_max", "9007199254740991", R"("9007199254740991")");
      accept<u128>("json", "bare_number_max", max128, max128);
      accept<u128>("json", "hex_string", R"("0xff")", "255");
      accept<u128>("ijson", "hex_string", R"("0xff")", R"("255")");
      accept<u128>("ijson", "hex_string_upper", R"("0XFF")", R"("255")");
      accept<u128>("ijson", "hex_string_max", R"("0xffffffffffffffffffffffffffffffff")",
                   "\"" + max128 + "\"");
      accept<i128>("ijson", "hex_string_negative", R"("-0x2a")", R"("-42")");
      accept<i128>("json", "hex_string_negative", R"("-0x2a")", "-42");
      accept<i128>("ijson", "bare_number_negative", "-9007199254740991", R"("-9007199254740991")");
      accept<i128>("ijson", "hex_string_min", R"("-0x80000000000000000000000000000000")",
                   R"("-170141183460469231731687303715884105728")");
      accept<u256>("json", "decimal_string", "\"" + max256 + "\"", max256);
      accept<u256>("ijson", "bare_number_small", "12", R"("12")");
      accept<u256>("ijson", "hex_string", R"("0x100")", R"("256")");
      accept<u256>("json", "hex_string", R"("0x100")", "256");

      reject<u128>("ijson", "bare_number_over_safe", "9007199254740992");
      reject<i128>("ijson", "bare_number_under_safe", "-9007199254740992");
      reject<u256>("ijson", "bare_number_over_safe", "9007199254740992");
      for (const char* f : {"json", "ijson"})
      {
         reject<u128>(f, "over_max", "\"340282366920938463463374607431768211456\"");
         reject<u128>(f, "negative", R"("-1")");
         reject<u128>(f, "leading_zero", R"("01")");
         reject<u128>(f, "empty_string", R"("")");
         reject<u128>(f, "plus", R"("+1")");
         reject<u128>(f, "space", R"(" 1")");
         reject<u128>(f, "fraction", "1.5");
         reject<u128>(f, "exponent", "1e3");
         reject<u128>(f, "hex_without_digits", R"("0x")");
         reject<u128>(f, "hex_over_max", R"("0x100000000000000000000000000000000")");
         reject<u128>(f, "hex_negative", R"("-0x1")");
         reject<u128>(f, "hex_bad_digit", R"("0xfg")");
         reject<u128>(f, "bool", "true");
         reject<u128>(f, "null", "null");
         reject<i128>(f, "over_max", "\"170141183460469231731687303715884105728\"");
         reject<i128>(f, "under_min", "\"-170141183460469231731687303715884105729\"");
         reject<i128>(f, "minus_zero", R"("-0")");
         reject<i128>(f, "hex_over_max", R"("0x80000000000000000000000000000000")");
         reject<i128>(f, "hex_under_min", R"("-0x80000000000000000000000000000001")");
         reject<u256>(f, "over_max",
                      "\"115792089237316195423570985008687907853269984665640564039457584007913129639936\"");
         reject<u256>(f, "negative", R"("-1")");
         reject<u256>(f, "leading_zero", R"("00")");
         reject<u256>(f, "hex_over_max",
                      R"("0x10000000000000000000000000000000000000000000000000000000000000000")");
      }
      reject<u128>("json", "bare_over_max", "340282366920938463463374607431768211456");
      reject<u128>("json", "bare_negative", "-1");
      reject<u128>("json", "bare_leading_zero", "01");
      reject<u256>("json", "bare_over_max",
                   "115792089237316195423570985008687907853269984665640564039457584007913129639936");
   }

   // Shortest round-trip edge cases: a value halfway between two equally
   // short candidates (2^-24), subnormals, and the magnitudes where a
   // JavaScript number changes notation (1e21, 1e-7).
   void emit_float_edges()
   {
      value<double>("2^-24", 0x1p-24);
      value<double>("minus_2^-24", -0x1p-24);
      value<double>("2^-1074", 0x1p-1074);
      value<double>("2^-1073", 0x1p-1073);
      value<double>("max_subnormal", 0x0.fffffffffffffp-1022);
      value<double>("min_normal", 0x1p-1022);
      value<double>("just_below_1e21", 999999999999999868928.0);
      value<double>("just_above_1e21", 1000000000000000131072.0);
      value<double>("1e20", 1e20);
      value<double>("1.5e21", 1.5e21);
      value<double>("123456789012345680000", 123456789012345680000.0);
      value<double>("1e-6", 1e-6);
      value<double>("1.5e-7", 1.5e-7);
      value<double>("just_below_1e-6", 9.999999999999999e-7);
      value<double>("just_above_1e-7", 1.0000000000000001e-7);
      value<double>("0.000001234", 0.000001234);
      value<double>("1e-5", 1e-5);
      value<double>("1e23", 1e23);
      value<double>("5e-324_times_3", 1.5e-323);
      value<double>("2^63", 0x1p63);
      value<double>("2^64", 0x1p64);
      value<double>("2^70", 0x1p70);
      value<double>("0.3", 0.3);
      value<double>("2^-10", 0x1p-10);
      value<float>("2^-24", 0x1p-24f);
      value<float>("2^-149", 0x1p-149f);
      value<float>("max_subnormal", 0x0.fffffep-126f);
      value<float>("min_normal", 0x1p-126f);
      value<float>("1e21", 1e21f);
      value<float>("1e20", 1e20f);
      value<float>("1e-7", 1e-7f);
      value<float>("1e-6", 1e-6f);
      value<float>("9.999999e-8", 9.999999e-8f);
      value<float>("2^63", 0x1p63f);
      value<float>("2^-10", 0x1p-10f);
      value<float>("0.3", 0.3f);
      value<float>("16777217_rounds", 16777217.0f);
      value<float>("3.4e38", 3.4e38f);
   }

   // Floats whose text must agree digit for digit across languages: random
   // bit patterns, and short decimals across the fixed / scientific boundary.
   void emit_float_sweep()
   {
      std::uint64_t state = 0x9E3779B97F4A7C15ull;
      auto next = [&] {
         std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
         z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
         z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
         return z ^ (z >> 31);
      };
      for (int i = 0; i < 150; ++i)
      {
         const std::uint64_t bits = next();
         double              d;
         std::memcpy(&d, &bits, sizeof d);
         if (std::isfinite(d)) value<double>("bits_" + std::to_string(i), d);
      }
      for (int i = 0; i < 100; ++i)
      {
         const std::uint32_t bits = static_cast<std::uint32_t>(next());
         float               f;
         std::memcpy(&f, &bits, sizeof f);
         if (std::isfinite(f)) value<float>("bits_" + std::to_string(i), f);
      }
      for (int i = 0; i < 160; ++i)
      {
         const std::uint64_t r        = next();
         const int           digits   = 1 + static_cast<int>(r % 6);
         std::uint64_t       mantissa = (r >> 8) % 1000000;
         for (int k = digits; k < 6; ++k) mantissa /= 10;
         const int         exponent = static_cast<int>((r >> 40) % 49) - 24;
         const std::string token =
            std::string((r >> 60) & 1 ? "-" : "") + std::to_string(mantissa) + "e" +
            std::to_string(exponent);
         value<double>("decimal_" + std::to_string(i), std::strtod(token.c_str(), nullptr));
         if (i % 2 == 0)
            value<float>("decimal_" + std::to_string(i), std::strtof(token.c_str(), nullptr));
      }
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
      value<Release>("empty", Release{});
      emit_custom();

      value<Tree>("leaf", tree_value(0));
      value<Tree>("three_levels", tree_value(3));
      // Deepest tree within kMaxValidationDepth = 64 nested containers.
      value<Tree>("depth_limit_31_levels", tree_value(31));
   }

   void emit_bounds_layout_caps()
   {
      const Bounds full{psio::utf8_string<8>{"12345678"}, {1, 2, 3}, "memo", {0xab, 0xcd},
                        std::string("abc")};
      value<Bounds>("empty", Bounds{});
      value<Bounds>("at_every_bound", full);
      value<Bounds>("some", Bounds{psio::utf8_string<8>{"n"}, {4294967295u}, "", {7}, std::string("")});
      value<Fixed>("zero", Fixed{});
      value<Fixed>("some", Fixed{-1, std::numeric_limits<std::int32_t>::max(), true});
      value<Color>("red", Color::red);
      value<Color>("blue", Color::blue);
      value<Color>("unnamed_255", static_cast<Color>(255));
      emit_enums();
      emit_wide_integers();
      emit_float_edges();
      value<Capped>("empty", Capped{});
      value<Capped>("some", Capped{9, "memo", Fixed{1, 2, true}, {Fixed{3, 4, false}, Fixed{}}});
      value<Capped>("trailing_none", Capped{1, "", std::nullopt, {}});
      value<Shapes>("zero", Shapes{});
      value<Shapes>("full", Shapes{Color::green, {{Fixed{1, 2, true}, Fixed{-3, -4, false}}}, 1.5f,
                                   -32768, text("label"), std::uint8_t{255},
                                   Capped{2, "m", Fixed{5, 6, true}, {Fixed{7, 8, true}}}, full});
      value<Text4>("empty", Text4{""});
      value<Text4>("at_bound", Text4{"abcd"});
      value<Text4>("two_byte_at_bound", Text4{"\xC3\xA9\xC3\xA9"});
      value<ListU16Max3>("empty", ListU16Max3{});
      value<ListU16Max3>("at_bound", ListU16Max3{1, 2, 65535});
      value<BytesMax2>("empty", BytesMax2{});
      value<BytesMax2>("at_bound", BytesMax2{0x01, 0xff});

      // ── Over a bound: rejected on read ─────────────────────────────────
      reject_both<Text4>("five_bytes", R"("abcde")");
      reject_both<Text4>("three_two_byte_characters", "\"\xC3\xA9\xC3\xA9\xC3\xA9\"");
      reject_both<Text4>("five_bytes_by_escape", "\"abcd" + u("0041") + "\"");
      reject_both<ListU16Max3>("four_elements", "[1,2,3,4]");
      reject<BytesMax2>("json", "three_bytes", "[1,2,3]");
      reject<BytesMax2>("ijson", "three_bytes", "\"010203\"");
      reject<BytesMax2>("ijson", "number_array", "[1,2]");
      reject<BytesMax2>("json", "hex_string", "\"0102\"");
      const std::string b_json  = R"({"name":"n","tags":[1],"memo":"m","blob":[7],"note":"abc"})";
      const std::string b_ijson = R"({"name":"n","tags":[1],"memo":"m","blob":"07","note":"abc"})";
      const Bounds      b{psio::utf8_string<8>{"n"}, {1}, "m", {7}, std::string("abc")};
      if (psio::encode(psio::json{}, b) != b_json || psio::encode(psio::ijson{}, b) != b_ijson)
         die("Bounds texts are stale");
      auto swap = [](std::string text, const std::string& from, const std::string& to) {
         const auto at = text.find(from);
         if (at == std::string::npos) die("Bounds reject row is stale: " + from);
         return text.replace(at, from.size(), to);
      };
      for (const bool ij : {false, true})
      {
         const char*        f    = ij ? "ijson" : "json";
         const std::string& base = ij ? b_ijson : b_json;
         reject<Bounds>(f, "name_over_wrapper_bound", swap(base, R"("name":"n")", R"("name":"123456789")"));
         reject<Bounds>(f, "tags_over_member_bound", swap(base, R"("tags":[1])", R"("tags":[1,2,3,4])"));
         reject<Bounds>(f, "memo_over_member_bound", swap(base, R"("memo":"m")", R"("memo":"12345")"));
         reject<Bounds>(f, "note_over_member_bound", swap(base, R"("note":"abc")", R"("note":"abcd")"));
         accept<Bounds>(f, "note_null", swap(base, R"("note":"abc")", R"("note":null)"),
                        swap(base, R"("note":"abc")", R"("note":null)"));
      }
      reject<Bounds>("json", "blob_over_member_bound", swap(b_json, R"("blob":[7])", R"("blob":[7,8,9])"));
      reject<Bounds>("ijson", "blob_over_member_bound", swap(b_ijson, R"("blob":"07")", R"("blob":"070809")"));
      // A repeated key is decoded each time: an over-bound earlier
      // occurrence is an error even though a later one replaces it.
      reject<Bounds>("json", "tags_over_bound_then_replaced",
                     R"({"name":"n","tags":[1,2,3,4],"tags":[1],"memo":"m","blob":[7],"note":null})");

      // The writers refuse a value outside a bound.
      auto refuses = [&](const std::string& what, const auto& v) {
         std::string       out = "x";
         std::vector<char> bytes{'x'};
         if (psio::encode_json_checked(psio::json{}, v, out).ok() || out != "x" ||
             psio::encode_json_checked(psio::ijson{}, v, out).ok() || out != "x")
            die("the JSON writer accepted " + what);
         if constexpr (has_pssz<std::remove_cvref_t<decltype(v)>>)
            if (psio::encode_pssz_checked(psio::pssz{}, v, bytes).ok() ||
                bytes != std::vector<char>{'x'})
               die("the pssz writer accepted " + what);
      };
      {
         Bounds over = b;
         over.tags   = {1, 2, 3, 4};
         refuses("tags over the member bound", over);
         over      = b;
         over.memo = "12345";
         refuses("memo over the member bound", over);
         over      = b;
         over.blob = {1, 2, 3};
         refuses("blob over the member bound", over);
         over      = b;
         over.note = "abcd";
         refuses("note over the member bound", over);
         ListU16Max3 list;
         list.storage() = {1, 2, 3, 4};
         refuses("a bounded list over its bound", list);
      }

      // Fixed layout, enum and nested shapes: wrong arity / range / form.
      reject_both<Fixed>("missing_member", R"({"x":1,"y":2})");
      reject_both<Fixed>("out_of_range", R"({"x":2147483648,"y":2,"on":true})");
      reject_both<Color>("out_of_range", "256");
      reject_both<Color>("negative", "-1");
      accept_both<Capped>("missing_optional_member", R"({"id":1,"memo":"","path":[]})",
                          R"({"id":1,"memo":"","at":null,"path":[]})");
      reject_both<Capped>("missing_required_member", R"({"id":1,"memo":""})");
      reject_both<Shapes>("corner_array_too_short",
                          R"({"color":0,"corners":[{"x":0,"y":0,"on":false}],"scale":0,"delta":0,"label":null,"small":null,"capped":{"id":0,"memo":"","at":null,"path":[]},"bounds":null})");
   }

   void emit_accepted()
   {
      // Bare numbers for wide integers under ijson, within the safe range.
      // Wide integers under ijson: a bare number within the safe range is
      // the canonical form there; the string form is accepted for any value.
      accept<u64>("ijson", "bare_number_small", "42", "42");
      accept<u64>("ijson", "bare_number_zero", "0", "0");
      accept<u64>("ijson", "bare_number_safe_max", "9007199254740991", "9007199254740991");
      accept<i64>("ijson", "bare_number_safe_min", "-9007199254740991", "-9007199254740991");
      accept<u64>("ijson", "string_small", "\"42\"", "42");
      accept<u64>("ijson", "string_zero", "\"0\"", "0");
      accept<u64>("ijson", "string_safe_max", "\"9007199254740991\"", "9007199254740991");
      accept<i64>("ijson", "string_safe_min", "\"-9007199254740991\"", "-9007199254740991");
      accept<i64>("ijson", "string_minus_one", "\"-1\"", "-1");
      accept<u64>("ijson", "string_2^53", "\"9007199254740992\"", "\"9007199254740992\"");
      accept<i64>("ijson", "string_-2^53", "\"-9007199254740992\"", "\"-9007199254740992\"");
      accept<Inner>("ijson", "bare_number_member", R"({"id":7,"label":"seven","flag":true})",
                    R"({"id":7,"label":"seven","flag":true})");
      accept<Inner>("ijson", "string_member", R"({"id":"7","label":"seven","flag":true})",
                    R"({"id":7,"label":"seven","flag":true})");
      accept<Vec<u64>>("ijson", "mixed_forms", R"([1,"2",3,"9007199254740992"])",
                       R"([1,2,3,"9007199254740992"])");

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
      // A repeated key is not an error in either format: each occurrence is
      // read as it is seen and the last one wins.
      accept_both<AllOptional>("duplicate_key_last_wins", R"({"a":1,"a":2})",
                               R"({"a":2,"b":null})");
      accept_both<AllOptional>("duplicate_key_same_value", R"({"a":1,"a":1})",
                               R"({"a":1,"b":null})");
      accept_both<AllOptional>("duplicate_key_escaped", "{\"a\":1,\"" + u("0061") + "\":3}",
                               R"({"a":3,"b":null})");
      accept_both<AllOptional>("duplicate_key_last_null", R"({"a":1,"b":"x","a":null})",
                               R"({"a":null,"b":"x"})");
      accept_both<AllOptional>("duplicate_unknown_key", R"({"z":1,"z":2})",
                               R"({"a":null,"b":null})");
      accept_both<AllOptional>("duplicate_unknown_key_escaped",
                               "{\"z\":1,\"" + u("007a") + "\":2}", R"({"a":null,"b":null})");
      accept_both<AllOptional>("duplicate_key_inside_unknown_value", R"({"z":{"k":1,"k":2}})",
                               R"({"a":null,"b":null})");
      accept_both<AllOptional>("duplicate_key_deep_inside_unknown_value",
                               R"({"z":[1,{"q":[{"k":1,"j":0,"k":2}]}]})",
                               R"({"a":null,"b":null})");
      accept<Inner>("ijson", "duplicate_member", R"({"id":"7","label":"x","flag":true,"id":"8"})",
                    R"({"id":8,"label":"x","flag":true})");
      accept<Inner>("json", "duplicate_member", R"({"id":7,"label":"x","flag":true,"id":8})",
                    R"({"id":8,"label":"x","flag":true})");
      accept<Holder>("ijson", "duplicate_key_in_nested_record",
                     R"({"choice":{"Inner":{"id":"1","id":"2","label":"","flag":false}},"choices":[]})",
                     R"({"choice":{"Inner":{"id":2,"label":"","flag":false}},"choices":[]})");
      // A repeated list member is replaced, not appended to.
      accept_both<Tree>("duplicate_list_member_replaced",
                        R"({"children":[{"children":[]},{"children":[]}],"children":[{"children":[]}]})",
                        R"({"children":[{"children":[]}]})");
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
      const std::string ov_ijson = R"({"a":1,"b":2,"c":"3","d":[4,5],"e":-6,"f":-7})";
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

      // ── Repeated keys: every occurrence is decoded as it is seen ────────
      // The last one wins (see the accepted rows), but an earlier,
      // overridden occurrence must still be a well-formed value of the
      // member's type.
      reject_both<AllOptional>("duplicate_key_earlier_wrong_type", R"({"a":"x","a":2})");
      reject_both<AllOptional>("duplicate_key_earlier_out_of_range", R"({"a":4294967296,"a":2})");
      reject_both<AllOptional>("duplicate_key_earlier_malformed", R"({"a":[1,,"a":2})");
      reject_both<AllOptional>("duplicate_key_later_wrong_type", R"({"a":2,"a":"x"})");
      reject_both<AllOptional>("duplicate_unknown_key_earlier_malformed", R"({"z":01,"z":2})");
      reject<Inner>("ijson", "duplicate_member_earlier_wrong_type",
                    R"({"id":true,"label":"x","flag":true,"id":"7"})");
      // A variant object has exactly one member, so a second key is an
      // error even when it repeats the first (see same_key_twice above).

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

   // ── Malformed schemas and their well-formed neighbours ─────────────────
   //
   // What is wrong with a rejected schema is its meaning, not its encoding:
   // each is a valid psio::Schema value. All three languages refuse the same
   // ones at every schema-driven entry point.
   namespace st = psio::schema_types;
   std::vector<SchemaRejectRow> schema_rejects;
   std::vector<SchemaAcceptRow> schema_accepts;

   st::Box<st::AnyType> box_of(st::AnyType type) { return st::Box<st::AnyType>{std::move(type)}; }
   st::AnyType          sint(std::uint32_t bits, bool is_signed) { return st::Int{bits, is_signed}; }
   st::AnyType          named(std::string name) { return st::Type{std::move(name)}; }
   st::AnyType custom(st::AnyType inner, std::string id)
   {
      return st::Custom{box_of(std::move(inner)), std::move(id)};
   }
   st::AnyType list(st::AnyType element) { return st::List{box_of(std::move(element))}; }
   st::AnyType enumeration(st::AnyType under)
   {
      return st::Enum{box_of(std::move(under)),
                      {st::Enumerator{"pending", static_cast<std::uint64_t>(-1)},
                       st::Enumerator{"active", 2}}};
   }
   using Named = std::vector<std::pair<std::string, st::AnyType>>;
   psio::Schema schema_with(Named types)
   {
      psio::Schema schema;
      for (auto& [name, type] : types)
         schema.insert(name, std::move(type));
      return schema;
   }

   void schema_reject(const std::string& name, psio::Schema schema, const std::string& root = "T")
   {
      namespace ir = psio::pssz_ir;
      const st::AnyType type{st::Type{root}};
      std::size_t       width = 0;
      if (ir::auto_width(schema, type, width).ok())
         die("schema_rejects/" + name + ": a width was derived");
      for (std::size_t n : {0u, 1u, 4u, 8u})
      {
         const std::vector<char> bytes(n, '\0');
         std::string             text;
         if (ir::validate_pssz(schema, type, std::span<const char>{bytes}).ok() ||
             ir::pssz_to_json(schema, type, psio::json{}, std::span<const char>{bytes}, text).ok())
            die("schema_rejects/" + name + ": bytes were accepted");
      }
      for (const std::string text : {"0", "\"\"", "null", "[]", "{}", "false"})
      {
         std::vector<char> out;
         if (ir::json_to_pssz(schema, type, psio::json{}, std::span<const char>{text}, out).ok() ||
             ir::json_to_pssz(schema, type, psio::ijson{}, std::span<const char>{text}, out).ok())
            die("schema_rejects/" + name + ": text was accepted");
      }
      schema_rejects.push_back({name, lower_hex(psio::encode(psio::pssz{}, schema)), root});
   }
   // `ijson`: the I-JSON text of the row's value.
   void schema_accept(const std::string& name, psio::Schema schema, const std::string& ijson,
                      const std::string& root = "T")
   {
      namespace ir = psio::pssz_ir;
      const st::AnyType type{st::Type{root}};
      std::vector<char> bytes, again;
      std::string       back, json;
      if (!ir::json_to_pssz(schema, type, psio::ijson{}, std::span<const char>{ijson}, bytes).ok() ||
          !ir::validate_pssz(schema, type, std::span<const char>{bytes}).ok() ||
          !ir::pssz_to_json(schema, type, psio::ijson{}, std::span<const char>{bytes}, back).ok() ||
          back != ijson ||
          !ir::pssz_to_json(schema, type, psio::json{}, std::span<const char>{bytes}, json).ok() ||
          !ir::json_to_pssz(schema, type, psio::json{}, std::span<const char>{json}, again).ok() ||
          again != bytes)
         die("schema_accepts/" + name + ": does not round-trip (ijson " + back + ", json " + json +
             ")");
      schema_accepts.push_back({name, lower_hex(psio::encode(psio::pssz{}, schema)), root,
                                lower_hex(bytes), json, ijson});
   }

   void emit_schema_rows()
   {
      const auto u8 = [&] { return sint(8, false); };
      const auto i32 = [&] { return sint(32, true); };
      const auto i64 = [&] { return sint(64, true); };
      const auto bytes = [&] { return list(u8()); };

      // An Enum sits directly on an integer (8 to 64 bits to have text).
      schema_accept("enum", schema_with({{"T", enumeration(i32())}}), "\"active\"");
      schema_accept("enum_over_alias",
                    schema_with({{"I", i32()}, {"J", named("I")}, {"T", enumeration(named("J"))}}),
                    "\"pending\"");
      schema_accept("enum_unnamed_value", schema_with({{"T", enumeration(sint(8, true))}}), "7");
      schema_reject("enum_over_enum", schema_with({{"T", enumeration(enumeration(i32()))}}));
      schema_reject("enum_over_alias_to_enum",
                    schema_with({{"E", enumeration(i32())}, {"T", enumeration(named("E"))}}));
      schema_reject("enum_over_box", schema_with({{"T", enumeration(custom(i32(), "box"))}}));
      schema_reject("enum_over_presentation",
                    schema_with({{"T", enumeration(custom(i32(), "AccountNumber"))}}));
      schema_reject("enum_over_time",
                    schema_with({{"T", enumeration(custom(i64(), "duration-millis"))}}));
      schema_reject("enum_over_bool",
                    schema_with({{"T", enumeration(custom(sint(1, false), "bool"))}}));
      schema_reject("enum_over_char", schema_with({{"T", enumeration(custom(u8(), "char"))}}));
      schema_reject("enum_over_float", schema_with({{"T", enumeration(st::Float{11, 53})}}));
      schema_reject("enum_over_string",
                    schema_with({{"T", enumeration(custom(bytes(), "utf8"))}}));
      schema_reject("enum_over_list", schema_with({{"T", enumeration(list(i32()))}}));
      schema_reject("enum_over_itself", schema_with({{"T", enumeration(named("T"))}}));

      // A byte adapter sits directly on a byte container of plain bytes.
      schema_accept("utf8", schema_with({{"T", custom(bytes(), "utf8")}}), "\"h\xc3\xa9\"");
      schema_accept("utf8_over_alias",
                    schema_with({{"B", bytes()}, {"T", custom(named("B"), "utf8")}}), "\"hi\"");
      schema_accept("string_over_aliased_element",
                    schema_with({{"U", u8()}, {"T", custom(list(named("U")), "string")}}),
                    "\"raw\"");
      schema_accept("hex", schema_with({{"T", custom(bytes(), "hex")}}), "\"00FF\"");
      for (const char* id : {"string", "utf8", "hex"})
      {
         const std::string n = id;
         schema_reject(n + "_over_box", schema_with({{"T", custom(custom(bytes(), "box"), id)}}));
         schema_reject(n + "_over_presentation",
                       schema_with({{"T", custom(custom(bytes(), "Name"), id)}}));
         schema_reject(n + "_over_alias_to_box",
                       schema_with({{"B", custom(bytes(), "box")}, {"T", custom(named("B"), id)}}));
         schema_reject(n + "_over_boxed_elements",
                       schema_with({{"T", custom(list(custom(u8(), "box")), id)}}));
         schema_reject(n + "_over_enum_elements",
                       schema_with({{"T", custom(list(enumeration(u8())), id)}}));
         schema_reject(n + "_over_integer", schema_with({{"T", custom(i32(), id)}}));
      }
      schema_reject("utf8_over_utf8",
                    schema_with({{"T", custom(custom(bytes(), "utf8"), "utf8")}}));
      schema_reject("bool_over_box",
                    schema_with({{"T", custom(custom(sint(1, false), "box"), "bool")}}));
      schema_reject("char_over_enum", schema_with({{"T", custom(enumeration(u8()), "char")}}));
      schema_reject("bitvector_over_box",
                    schema_with({{"T", custom(custom(st::Array{box_of(sint(1, false)), 8}, "box"),
                                              "bitvector")}}));

      // A wrapper around a well-formed type is fine: these are looked through.
      schema_accept("box_over_enum", schema_with({{"T", custom(enumeration(i32()), "box")}}),
                    "\"active\"");
      schema_accept("presentation_over_utf8",
                    schema_with({{"T", custom(custom(bytes(), "utf8"), "Name")}}), "\"n\"");
      schema_accept("box_over_box_over_hex",
                    schema_with({{"T", custom(custom(custom(bytes(), "hex"), "box"), "box")}}),
                    "\"AB\"");
      schema_accept("list_of_enum", schema_with({{"T", list(enumeration(i32()))}}),
                    "[\"active\",5]");

      // A time id sits directly on its tick integer; its spellings are reserved.
      schema_accept("time_point", schema_with({{"T", custom(i64(), "time-point-millis")}}),
                    "\"2026-11-22T10:15:30.123Z\"");
      schema_accept("duration_over_alias",
                    schema_with({{"I", i64()}, {"T", custom(named("I"), "duration-60/1")}}), "-5");
      schema_accept("box_over_time",
                    schema_with({{"T", custom(custom(i64(), "time-point-seconds"), "box")}}),
                    "\"1970-01-01T00:00:00Z\"");
      schema_reject("time_over_box",
                    schema_with({{"T", custom(custom(i64(), "box"), "time-point-micros")}}));
      schema_reject("time_over_enum",
                    schema_with({{"T", custom(enumeration(i64()), "duration-millis")}}));
      schema_reject("time_over_time",
                    schema_with({{"T", custom(custom(i64(), "duration-millis"), "duration-millis")}}));
      schema_reject("time_over_int128",
                    schema_with({{"T", custom(sint(128, true), "duration-millis")}}));
      schema_reject("time_over_float",
                    schema_with({{"T", custom(st::Float{11, 53}, "time-point-seconds")}}));
      for (const char* id : {"time-point", "duration", "time-point-", "duration-",
                             "duration-minutes", "duration-1/1000", "duration-2/4",
                             "time-point-01/7", "duration-0/1"})
         schema_reject(std::string("reserved_time_id/") + id, schema_with({{"T", custom(i64(), id)}}));

      // The malformed node is refused wherever the root reaches it.
      schema_reject("member_enum_over_enum",
                    schema_with({{"T", st::Object{{st::Member{"ok", box_of(i32()), {}},
                                                   st::Member{"bad",
                                                              box_of(enumeration(enumeration(i32()))),
                                                              {}}},
                                                  {}}}}));
      schema_reject("optional_list_string_over_box",
                    schema_with({{"T", st::Option{box_of(
                                          list(custom(custom(bytes(), "box"), "string")))}}}));

      // The IR version marker (schema_ir.hpp): absent or unknown is refused.
      {
         auto old = schema_with({{"T", i32()}});
         schema_accept("ir_version_current", old, "-7");
         old.ir_version.reset();
         schema_reject("ir_version_absent", old);
         old.ir_version = psio::schema_ir_version + 1;
         schema_reject("ir_version_newer", old);
         old.ir_version = 0;
         schema_reject("ir_version_zero", old);
      }
   }

   // Transparent wrappers, custom text forms of any shape, invariants
   // (custom_conformance_types.hpp; conformance/custom-types.md).
   void emit_custom_types()
   {
      using namespace custom_conf;
      const auto name60 = (std::uint64_t{1} << 60);

      // ── Transparent: the member's text ──────────────────────────────────
      value<Id>("zero", Id{0});
      value<Id>("small", Id{42});
      value<Id>("max", Id{~std::uint64_t{0}});
      reject_both<Id>("as_record", R"({"value":7})");
      reject_both<Id>("string_name", R"("seven")");
      reject_both<Id>("negative", "-1");
      accept<Id>("ijson", "string_form", R"("42")", "42");

      // ── A transparent wrapper with a text form: base64 ──────────────────
      value<Blob>("empty", Blob{});
      value<Blob>("one", Blob{{0x00}});
      value<Blob>("two", Blob{{0xff, 0xfe}});
      value<Blob>("three", Blob{{'M', 'a', 'n'}});
      value<Blob>("many", Blob{{0xfb, 0xff, 0xbf, 0x00, 0x10, 0x83, 0x10}});
      reject_both<Blob>("not_a_string", "[1,2]");
      reject_both<Blob>("bad_length", R"("TQ")");
      reject_both<Blob>("bad_character", R"("TW_u")");
      reject_both<Blob>("padding_inside", R"("T=Fu")");
      reject_both<Blob>("three_padding", R"("T===")");
      reject_both<Blob>("unused_bits_one_pad", R"("TWF=")");
      reject_both<Blob>("unused_bits_two_pad", R"("TR==")");
      reject_both<Blob>("whitespace", R"("TWFu ")");

      // ── A variant with a text form ──────────────────────────────────────
      V6 loopback;
      loopback.address[15] = 1;
      loopback.port        = 443;
      V6 mixed{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xff, 0x00, 0, 0x42, 0x83, 0x29}, 65535};
      value<Endpoint>("v4_zero", Endpoint{V4{}});
      value<Endpoint>("v4_local", Endpoint{V4{{127, 0, 0, 1}, 8080}});
      value<Endpoint>("v4_max", Endpoint{V4{{255, 255, 255, 255}, 65535}});
      value<Endpoint>("v6_zero", Endpoint{V6{}});
      value<Endpoint>("v6_loopback", Endpoint{loopback});
      value<Endpoint>("v6_mixed", Endpoint{mixed});
      value<std::vector<Endpoint>>("both", std::vector<Endpoint>{V4{{10, 0, 0, 7}, 1}, loopback});
      reject_both<Endpoint>("no_port", R"("127.0.0.1")");
      reject_both<Endpoint>("port_too_large", R"("127.0.0.1:65536")");
      reject_both<Endpoint>("port_leading_zero", R"("127.0.0.1:080")");
      reject_both<Endpoint>("octet_too_large", R"("256.0.0.1:80")");
      reject_both<Endpoint>("octet_leading_zero", R"("127.00.0.1:80")");
      reject_both<Endpoint>("three_octets", R"("127.0.1:80")");
      reject_both<Endpoint>("v6_shorthand", R"("[::1]:443")");
      reject_both<Endpoint>("v6_upper_case",
                            R"("[0000:0000:0000:0000:0000:0000:0000:000A]:443")");
      reject_both<Endpoint>("v6_seven_groups", R"("[0000:0000:0000:0000:0000:0000:0001]:443")");
      reject_both<Endpoint>("v6_no_brackets",
                            R"("0000:0000:0000:0000:0000:0000:0000:0001:443")");
      reject_both<Endpoint>("empty", R"("")");
      reject_both<Endpoint>("trailing", R"("127.0.0.1:80 ")");
      // The variant's own forms are not the type's text.
      reject_both<Endpoint>("variant_array_form", R"([0,{"address":[127,0,0,1],"port":80}])");
      reject_both<Endpoint>("variant_object_form", R"({"V4":{"address":"7F000001","port":80}})");

      // ── A tuple with a text form ────────────────────────────────────────
      value<Ratio>("zero", Ratio{0, 0});
      value<Ratio>("negative", Ratio{-3, 4});
      value<Ratio>("extremes_low", Ratio{-32768, 65535});
      value<Ratio>("extremes_high", Ratio{32767, 1});
      reject_both<Ratio>("tuple_form", "[-3,4]");
      reject_both<Ratio>("no_denominator", R"("3")");
      reject_both<Ratio>("negative_zero", R"("-0/1")");
      reject_both<Ratio>("numerator_too_large", R"("32768/1")");
      reject_both<Ratio>("numerator_too_small", R"("-32769/1")");
      reject_both<Ratio>("denominator_too_large", R"("1/65536")");
      reject_both<Ratio>("negative_denominator", R"("1/-2")");
      reject_both<Ratio>("leading_zero", R"("03/4")");
      reject_both<Ratio>("plus_sign", R"("+3/4")");
      reject_both<Ratio>("spaces", R"("3 / 4")");

      // ── A transparent variant wrapper: text form and invariant ──────────
      value<Ref>("number_zero", Ref{std::uint64_t{0}});
      value<Ref>("number_max", Ref{~std::uint64_t{0}});
      value<Ref>("name", Ref{std::string{"alice"}});
      value<Ref>("name_with_marks", Ref{std::string{"#@ x"}});
      reject_both<Ref>("empty", R"("")");
      reject_both<Ref>("no_marker", R"("alice")");
      reject_both<Ref>("number_leading_zero", R"("#07")");
      reject_both<Ref>("number_too_large", R"("#18446744073709551616")");
      reject_both<Ref>("number_empty", R"("#")");
      reject_both<Ref>("variant_form", R"([1,"alice"])");
      // "@" is well-formed text: the empty name, which the invariant refuses.
      invariant<Ref>("empty_name", "Ref: a name is not empty", R"([1,""])", R"("@")", R"("@")");

      // ── Invariants ──────────────────────────────────────────────────────
      value<Name>("zero", Name{0});
      value<Name>("largest", Name{name60 - 1});
      invariant<Name>("first_non_canonical", "Name: value is not a canonical name",
                      R"({"value":1152921504606846976})");
      invariant<Name>("all_ones", "Name: value is not a canonical name",
                      R"({"value":18446744073709551615})");
      reject_both<Name>("bare_number", "7");

      value<DerKey>("minimal", DerKey{{0x30}});
      value<DerKey>("sequence", DerKey{{0x30, 0x03, 0x02, 0x01, 0x05}});
      invariant<DerKey>("empty", "DerKey: not a DER sequence", "[]");
      invariant<DerKey>("wrong_tag", "DerKey: not a DER sequence", "[49,0]");
      reject_both<DerKey>("as_record", R"({"data":[48]})");

      value<Range>("empty_range", Range{0, 0});
      value<Range>("ordered", Range{3, 9});
      value<Range>("widest", Range{0, 4294967295u});
      invariant<Range>("reversed", "Range: lo is greater than hi", R"({"lo":9,"hi":3})");
      invariant<Range>("off_by_one", "Range: lo is greater than hi", R"({"lo":1,"hi":0})");

      value<Level>("low", Level::low);
      value<Level>("top", Level::top);
      invariant<Level>("unlisted", "Level: not a listed level", "4");
      invariant<Level>("largest", "Level: not a listed level", "255");
      reject_both<Level>("unknown_name", R"("extreme")");
      value<std::vector<Level>>("empty", std::vector<Level>{});
      value<std::vector<Level>>("all", std::vector<Level>{Level::low, Level::mid, Level::high,
                                                          Level::top});
      invariant<std::vector<Level>>("second_unlisted", "Level: not a listed level",
                                    R"(["low",9,"top"])");
      value<std::optional<Name>>("none", std::optional<Name>{});
      value<std::optional<Name>>("some", std::optional<Name>{Name{9}});
      invariant<std::optional<Name>>("some_non_canonical",
                                     "Name: value is not a canonical name",
                                     R"({"value":1152921504606846977})");

      // ── Everything, in every position ───────────────────────────────────
      const Guarded good = guarded_sample();
      value<Guarded>("sample", good);
      const std::string plain_json  = *rows.back().plain_json;
      const std::string typed_json  = *rows.back().json;
      const std::string typed_ijson = *rows.back().ijson;
      {
         Guarded g = good;
         g.delegate.reset();
         g.members.clear();
         g.levels.clear();
         g.either = Name{1};
         g.ref    = Ref{std::uint64_t{7}};
         g.blob.reset();
         g.endpoint = loopback;
         g.ratios.clear();
         value<Guarded>("other_alternatives", g);
      }
      // One member at a time made to violate its invariant: the texts of
      // `sample` with that member's text replaced — in the wire-type text
      // (what a codec with no handler reads), and in the typed json and
      // ijson texts.
      struct Swap
      {
         std::string from, to;
      };
      auto swapped = [&](std::string text, const Swap& swap) {
         const auto at = text.find(swap.from);
         if (at == std::string::npos || text.find(swap.from, at + 1) != std::string::npos)
            die("guarded: '" + swap.from + "' does not occur exactly once in " + text);
         text.replace(at, swap.from.size(), swap.to);
         return text;
      };
      auto guarded = [&](const std::string& name, const std::string& reason, const Swap& plain,
                         const Swap& json, const Swap& ijson) {
         invariant<Guarded>(name, reason, swapped(plain_json, plain), swapped(typed_json, json),
                            swapped(typed_ijson, ijson));
      };
      const char* canonical = "Name: value is not a canonical name";
      {
         const Swap json{R"("owner":{"value":5})", R"("owner":{"value":1152921504606846976})"};
         guarded("owner", canonical, json, json,
                 {R"("owner":{"value":5})", R"("owner":{"value":"1152921504606846976"})"});
      }
      {
         const Swap json{R"("delegate":{"value":1152921504606846975})",
                         R"("delegate":{"value":1152921504606846976})"};
         guarded("delegate", canonical, json, json,
                 {R"("delegate":{"value":"1152921504606846975"})",
                  R"("delegate":{"value":"1152921504606846976"})"});
      }
      {
         const Swap json{R"({"value":77}])", R"({"value":2305843009213693952}])"};
         guarded("member", canonical, json, json,
                 {R"({"value":77}])", R"({"value":"2305843009213693952"}])"});
      }
      {
         const Swap json{R"("key":[48,3,1,2,3])", R"("key":[4,3,1,2,3])"};
         guarded("key", "DerKey: not a DER sequence", json, json,
                 {R"("key":"3003010203")", R"("key":"0403010203")"});
      }
      {
         const Swap any{R"("window":{"lo":3,"hi":9})", R"("window":{"lo":10,"hi":9})"};
         guarded("window", "Range: lo is greater than hi", any, any, any);
      }
      {
         const Swap any{R"("levels":["low","top"])", R"("levels":["low",200])"};
         guarded("level_in_list", "Level: not a listed level", any, any, any);
      }
      {
         const Swap any{R"("pair":["mid","high"])", R"("pair":[4,"high"])"};
         guarded("level_in_array", "Level: not a listed level", any, any, any);
      }
      {
         const Swap json{R"("either":[1,{"lo":1,"hi":1}])", R"("either":[1,{"lo":2,"hi":1}])"};
         guarded("variant_alternative", "Range: lo is greater than hi", json, json,
                 {R"("either":{"Range":{"lo":1,"hi":1}})", R"("either":{"Range":{"lo":2,"hi":1}})"});
      }
      // The member with a text form: its wire-type text and its typed text.
      guarded("ref", "Ref: a name is not empty", {R"("ref":[1,"alice"])", R"("ref":[1,""])"},
              {R"("ref":"@alice")", R"("ref":"@")"}, {R"("ref":"@alice")", R"("ref":"@")"});
   }

   std::string render()
   {
      std::string types;
      for_each_type([&]<typename T>(std::type_identity<T>) {
         types += std::string(types.empty() ? "" : ",\n") + "{\"id\":" + quoted(type_id<T>()) +
                  ",\"ir\":" + (has_ir<T> ? "true" : "false") + ",\"pssz\":" +
                  (has_pssz<T> ? "true" : "false");
         // The offset width the typed psio::pssz codec uses for T: what a
         // schema-only reader must derive from the IR.
         if constexpr (has_pssz<T>)
            types += ",\"auto_width\":" + std::to_string(psio::auto_pssz_width_v<T>);
         // Rows of this type also carry plain_json / plain_ijson.
         if constexpr (has_custom<T>)
            types += ",\"custom\":true";
         // The type holds an invariant somewhere: "invariant" rows exist.
         if constexpr (has_invariant<T>)
            types += ",\"invariant\":true";
         types += "}";
      });
      const psio::Schema& schema = fixture_schema();
      std::string out = "{\"version\":1,\"schema_ir\":\"" +
                        lower_hex(psio::encode(psio::pssz{}, schema)) + "\",\"types\":[\n" +
                        types + "\n],\"vectors\":[\n";
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
         field("pssz", r.pssz);
         field("plain_json", r.plain_json);
         field("plain_ijson", r.plain_ijson);
         field("reason", r.reason);
         out += i + 1 < rows.size() ? "},\n" : "}\n";
      }
      out += "],\"schema_rejects\":[\n";
      for (std::size_t i = 0; i < schema_rejects.size(); ++i)
      {
         const auto& r = schema_rejects[i];
         out += "{\"name\":" + quoted(r.name) + ",\"schema_ir\":" + quoted(r.schema_ir) +
                ",\"root\":" + quoted(r.root) + (i + 1 < schema_rejects.size() ? "},\n" : "}\n");
      }
      out += "],\"schema_accepts\":[\n";
      for (std::size_t i = 0; i < schema_accepts.size(); ++i)
      {
         const auto& r = schema_accepts[i];
         out += "{\"name\":" + quoted(r.name) + ",\"schema_ir\":" + quoted(r.schema_ir) +
                ",\"root\":" + quoted(r.root) + ",\"pssz\":" + quoted(r.pssz) + ",\"json\":" +
                quoted(r.json) + ",\"ijson\":" + quoted(r.ijson) +
                (i + 1 < schema_accepts.size() ? "},\n" : "}\n");
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
   emit_float_sweep();
   emit_strings_and_bytes();
   emit_containers();
   emit_records();
   emit_bounds_layout_caps();
   emit_accepted();
   emit_rejected();
   emit_schema_rows();
   emit_custom_types();

   const std::string text = render();
   // The fixture is itself read back with the codec under test.
   Fixture parsed;
   if (!psio::decode_json_checked(psio::json{}, std::span<const char>{text}, parsed).ok() ||
       parsed.vectors.size() != rows.size() ||
       parsed.schema_rejects.size() != schema_rejects.size() ||
       parsed.schema_accepts.size() != schema_accepts.size())
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
